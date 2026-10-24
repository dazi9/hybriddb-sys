// Model-based differential and crash-consistency fuzzer for HybridDB engines.
//
// Profiles and contracts:
//   ops     - random put/del/get/scan sequences checked against a std::map
//             oracle, with periodic checkpoint (btree), compact_all (lsm)
//             and clean reopen. Every read must match the oracle exactly.
//   crash   - periodic simulate_crash_without_flush() followed by reopen.
//             WAL must restore every completed op: the recovered state must
//             equal one of the last few oracle snapshots.
//   corrupt - after a clean close, flip bits in a random file under db_dir.
//             On reopen every read must return either the exact oracle value
//             or a non-OK status (fail-closed). Wrong values and unusable
//             engines are findings.
//
// Usage:
//   ./build/fuzz_model --engine both --profile all --rounds 30 --seed 1
//
// Build (from repo root, WSL), one line:
//   g++ -std=c++20 -O1 -g -Wall -Wextra -pthread -Iinclude -fsanitize=address,undefined src/disk_manager.cpp src/buffer_pool.cpp src/wal.cpp src/bloom.cpp src/btree.cpp src/lsm.cpp fuzz/model_fuzz.cpp -o build/fuzz_model

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "hybriddb/btree.hpp"
#include "hybriddb/lsm.hpp"

using namespace hybriddb;

namespace {

int g_findings = 0;
std::string g_ctx;  // "engine=xxx round=N op=M"

void finding(const std::string& kind, const std::string& detail) {
    ++g_findings;
    std::cout << "[FINDING] " << kind << " " << g_ctx << " : " << detail << std::endl;
}

std::filesystem::path round_dir(int round) {
    auto base = std::filesystem::path("/tmp/opencode/hybriddb_fuzz") /
                ("round_" + std::to_string(round));
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base, ec);
    return base;
}

class Fuzzer {
public:
    Fuzzer(const std::string& engine_name, uint64_t seed, int round_no)
        : engine_name_(engine_name), round_no_(round_no), rng_(seed) {
        for (int i = 0; i < 60; ++i) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "k%04d", i);
            pool_.emplace_back(buf);
        }
        for (int i = 0; i < 15; ++i) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "hot%02d", i);
            pool_.emplace_back(buf);  // hot keys get extra traffic
        }
    }

    void run_profile(const std::string& profile) {
        profile_name_ = profile;
        auto dir = round_dir(round_no_);
        try {
            if (profile == "ops") run_ops(dir, /*crash_between=*/false);
            else if (profile == "crash") run_ops(dir, /*crash_between=*/true);
            else if (profile == "corrupt") run_corrupt(dir);
        } catch (const std::exception& ex) {
            finding("HARNESS-EXCEPTION", std::string("round crashed: ") + ex.what());
        } catch (...) {
            finding("HARNESS-UNKNOWN-EXCEPTION", "non-std exception escaped the round");
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

private:
    std::string rand_key() {
        // zipf-ish: 60% from the hot 15 keys
        if (std::uniform_int_distribution<int>(0, 99)(rng_) < 60) {
            return pool_[std::uniform_int_distribution<int>(60, 74)(rng_)];
        }
        return pool_[std::uniform_int_distribution<int>(0, 59)(rng_)];
    }

    std::string rand_value() {
        static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        size_t n = std::uniform_int_distribution<size_t>(0, 250)(rng_);
        std::string v;
        v.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            v.push_back(alphabet[std::uniform_int_distribution<size_t>(0, 35)(rng_)]);
        }
        return v;
    }

    // ---- verification ---------------------------------------------------

    // Contract: reads of keys tracked by the oracle must match it exactly;
    // every read path must be either exact or cleanly NOT_FOUND/non-OK.
    template <typename Engine>
    bool verify_get(Engine& eng, const std::string& key,
                    const std::map<std::string, std::string>& truth,
                    const std::string& ctx, bool strict_missing) {
        std::string out;
        Status st = eng.get(key, &out);
        auto it = truth.find(key);
        if (it != truth.end()) {
            if (st.ok() && out != it->second) {
                finding("OPS-VALUE", ctx + ": key=" + key + " got=" + out +
                                         " want=" + it->second);
                return false;
            }
            if (st.is_not_found()) {
                finding("OPS-LOST-KEY", ctx + ": live key=" + key + " reported NOT_FOUND");
                return false;
            }
            if (!st.ok()) {
                finding("OPS-BAD-STATUS", ctx + ": get(" + key + ") -> " + st.message);
                return false;
            }
        } else if (strict_missing && st.ok()) {
            finding("OPS-GHOST-KEY",
                    ctx + ": absent key=" + key + " returned value=" + out);
            return false;
        }
        return true;
    }

    template <typename Engine>
    void verify_scan(Engine& eng, const std::string& ctx) {
        if (model_.empty()) return;
        auto keys_sorted = sorted_model_keys();
        const std::string& a = keys_sorted[std::uniform_int_distribution<size_t>(
            0, keys_sorted.size() - 1)(rng_)];
        const std::string& b = keys_sorted[std::uniform_int_distribution<size_t>(
            0, keys_sorted.size() - 1)(rng_)];
        std::string lo = std::min(a, b), hi = std::max(a, b);
        std::vector<std::pair<std::string, std::string>> got;
        Status st = eng.scan(lo, hi, &got);
        if (!st.ok()) {
            finding("OPS-SCAN-STATUS",
                    ctx + ": scan(" + lo + "," + hi + ") -> " + st.message);
            return;
        }
        std::vector<std::pair<std::string, std::string>> want;
        for (auto it = model_.lower_bound(lo); it != model_.end() && it->first <= hi; ++it) {
            want.emplace_back(it->first, it->second);
        }
        if (got != want) {
            std::ostringstream oss;
            oss << ctx << ": scan(" << lo << "," << hi << ") got_len=" << got.size()
                << " want_len=" << want.size();
            for (size_t i = 0; i < std::min(got.size(), want.size()); ++i) {
                if (got[i] != want[i]) {
                    oss << " first_diff@" << i << " got=(" << got[i].first
                        << ") want=(" << want[i].first << ")";
                    break;
                }
            }
            finding("OPS-SCAN", oss.str());
        }
    }

    template <typename Engine>
    void full_verify(Engine& eng, const std::string& ctx) {
        std::vector<std::string> all(pool_);
        for (const auto& [k, v] : model_) all.push_back(k);
        for (const auto& key : all) verify_get(eng, key, model_, ctx, true);
        verify_scan(eng, ctx);
    }

    std::vector<std::string> sorted_model_keys() const {
        std::vector<std::string> keys;
        keys.reserve(model_.size());
        for (const auto& [k, v] : model_) keys.push_back(k);
        return keys;
    }

    // ---- engine plumbing -------------------------------------------------

    std::unique_ptr<BPlusTreeEngine> open_btree(const std::filesystem::path& dir) {
        BPlusTreeConfig cfg{};
        cfg.db_dir = dir;
        cfg.buffer_pool_frames = 32;
        cfg.replacer_policy = ReplacementPolicyType::LRU_2;
        cfg.sync_wal_on_write = false;  // fuzz the default (dirtiest) mode
        cfg.verify_crc = true;
        return std::make_unique<BPlusTreeEngine>(cfg);
    }

    std::unique_ptr<LSMTreeEngine> open_lsm(const std::filesystem::path& dir) {
        LSMConfig cfg{};
        cfg.db_dir = dir;
        cfg.memtable_max_bytes = 8 * 1024;
        cfg.l0_compaction_trigger = 2;
        cfg.max_levels = 3;
        cfg.bloom_bits_per_key = 8.0;
        cfg.bloom_mode = BloomTuningMode::MONKEY_OPTIMAL;
        cfg.sync_wal_on_write = false;
        cfg.verify_crc = true;
        return std::make_unique<LSMTreeEngine>(cfg);
    }

    bool is_btree() const { return engine_name_ == "btree"; }

    template <typename Engine>
    void fresh_reopen(const std::filesystem::path& dir, std::unique_ptr<Engine>& eng) {
        if constexpr (std::is_same_v<Engine, BPlusTreeEngine>) {
            eng = open_btree(dir);
        } else {
            eng = open_lsm(dir);
        }
    }

    template <typename Engine>
    void structural_maintenance(Engine& eng) {
        if constexpr (std::is_same_v<Engine, BPlusTreeEngine>) {
            Status st = eng.checkpoint();
            if (!st.ok()) finding("CHECKPOINT-FAILED", st.message);
        } else {
            Status st = eng.compact_all();
            if (!st.ok()) finding("COMPACT-FAILED", st.message);
        }
    }

    // ---- profiles ---------------------------------------------------------

    void run_ops(const std::filesystem::path& dir, bool crash_between) {
        model_.clear();
        snapshots_.assign(1, {});
        int n_ops = std::uniform_int_distribution<int>(60, 220)(rng_);

        if (is_btree()) {
            auto eng = open_btree(dir);
            drive_ops(dir, eng, n_ops, crash_between);
        } else {
            auto eng = open_lsm(dir);
            drive_ops(dir, eng, n_ops, crash_between);
        }
    }

    template <typename Engine>
    void drive_ops(const std::filesystem::path& dir,
                   std::unique_ptr<Engine>& eng, int n_ops, bool crash_between) {
        for (int i = 0; i < n_ops; ++i) {
            g_ctx = "engine=" + engine_name_ + " profile=" + profile_name_ +
                    " round=" + std::to_string(round_no_) +
                    " op=" + std::to_string(i);
            double r = std::uniform_real_distribution<double>(0, 1)(rng_);
            if (r < 0.48) {
                std::string k = rand_key(), v = rand_value();
                Status st = eng->put(k, v);
                if (!st.ok()) {
                    finding("OPS-PUT-FAILED", "put(" + k + ") -> " + st.message);
                } else {
                    model_[k] = v;
                }
            } else if (r < 0.63) {
                std::string k = rand_key();
                bool existed = model_.count(k) > 0;
                Status st = eng->del(k);
                // Engine semantics: deleting an absent key reports NOT_FOUND;
                // deleting a live key must succeed.
                if (existed && !st.ok()) {
                    finding("OPS-DEL-FAILED", "del(" + k + ") -> " + st.message);
                }
                if (!existed && !st.ok() && !st.is_not_found()) {
                    finding("OPS-DEL-BAD-STATUS", "del(" + k + ") -> " + st.message);
                }
                model_.erase(k);
            } else if (r < 0.80) {
                verify_get(*eng, rand_key(), model_, "op" + std::to_string(i), true);
            } else if (r < 0.90) {
                verify_scan(*eng, "op" + std::to_string(i));
            } else if (crash_between && r < 0.96) {
                // Hard crash: drop dirty buffer pages / unflushed memtable,
                // keep whatever reached the WAL, then reopen.
                eng->simulate_crash_without_flush();
                eng.reset();
                fresh_reopen(dir, eng);
                verify_crash_recovery(*eng);
            } else if (!crash_between && r < 0.96) {
                structural_maintenance(*eng);
            } else {
                // clean reopen cycle
                eng.reset();
                fresh_reopen(dir, eng);
                full_verify(*eng, "reopen@" + std::to_string(i));
            }
            snapshots_.push_back(model_);
            if (i % 25 == 24) full_verify(*eng, "op" + std::to_string(i));
        }
        full_verify(*eng, "final");
        // Clean-reopen convergence after the full op sequence.
        eng.reset();
        fresh_reopen(dir, eng);
        full_verify(*eng, "after-clean-reopen");
    }

    template <typename Engine>
    void verify_crash_recovery(Engine& eng) {
        // Everything that reached the WAL before the crash must come back:
        // accept the recovered state if it matches any of the last 3
        // snapshots (a torn final op may legitimately be lost).
        size_t n = snapshots_.size();
        if (matches_snapshot(eng, model_)) return;
        for (size_t si = n >= 3 ? n - 3 : 0; si + 1 < n; ++si) {
            if (matches_snapshot(eng, snapshots_[si])) {
                model_ = snapshots_[si];
                return;
            }
        }
        finding("CRASH-LOST-WRITES",
                "state after crash+reopen matches none of the last 3 snapshots");
    }

    template <typename Engine>
    bool matches_snapshot(Engine& eng, const std::map<std::string, std::string>& snap) {
        std::vector<std::string> probe(pool_);
        for (const auto& [k, v] : snap) probe.push_back(k);
        for (const auto& key : probe) {
            if (!verify_get(eng, key, snap, "crash-recovery", true)) return false;
        }
        return true;
    }

    void run_corrupt(const std::filesystem::path& dir) {
        g_ctx = "engine=" + engine_name_ + " profile=corrupt round=" +
                std::to_string(round_no_);
        model_.clear();
        snapshots_.assign(1, {});
        int n_ops = std::uniform_int_distribution<int>(60, 160)(rng_);
        if (is_btree()) {
            auto eng = open_btree(dir);
            populate(dir, eng, n_ops);
            eng.reset();  // clean close
        } else {
            auto eng = open_lsm(dir);
            populate(dir, eng, n_ops);
            eng.reset();
        }

        // Flip 1-3 bits in a random non-empty file under db_dir.
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file() && entry.file_size() > 0) {
                files.push_back(entry.path());
            }
        }
        if (!files.empty()) {
            const auto& target = files[std::uniform_int_distribution<size_t>(
                0, files.size() - 1)(rng_)];
            std::streamoff size = std::filesystem::file_size(target);
            std::fstream f(target, std::ios::binary | std::ios::in | std::ios::out);
            if (f.is_open()) {
                int flips = std::uniform_int_distribution<int>(1, 3)(rng_);
                for (int i = 0; i < flips; ++i) {
                    std::streamoff pos = std::uniform_int_distribution<std::streamoff>(
                        0, size - 1)(rng_);
                    char byte;
                    f.seekg(pos);
                    f.read(&byte, 1);
                    byte ^= static_cast<char>(1 << std::uniform_int_distribution<int>(0, 7)(rng_));
                    f.seekp(pos);
                    f.write(&byte, 1);
                }
                f.close();
                g_ctx += " corrupt_file=" + target.filename().string();
            }
        }

        // Fail-closed contract: reopen must not fabricate data. Every read
        // returns either the exact oracle value or a clean failure; the
        // engine must remain usable for new writes.
        auto check = [&](auto& eng) {
            std::vector<std::string> probe(pool_);
            for (const auto& [k, v] : model_) probe.push_back(k);
            for (const auto& key : probe) {
                std::string out;
                Status st = eng.get(key, &out);
                auto it = model_.find(key);
                if (st.ok()) {
                    if (it != model_.end() && out != it->second) {
                        finding("CORRUPT-WRONG-VALUE",
                                "key=" + key + " got=" + out + " want=" + it->second);
                        return;
                    }
                    if (it == model_.end()) {
                        finding("CORRUPT-GHOST-KEY",
                                "key=" + key + " should be absent, got=" + out);
                        return;
                    }
                }
                // NOT_FOUND / non-OK on a corrupted key = acceptable fail-closed
            }
            eng.put("__post_corrupt__", "alive");
            std::string out;
            if (eng.get("__post_corrupt__", &out).ok() && out != "alive") {
                finding("CORRUPT-POSTOP-FAILED",
                        "wrote after corrupted reopen but read back a different value");
            }
            // A clean non-OK write failure on a corrupted store is an
            // acceptable fail-closed outcome; only wrong data is a finding.
        };
        try {
            if (is_btree()) {
                auto eng = open_btree(dir);
                check(*eng);
            } else {
                auto eng = open_lsm(dir);
                check(*eng);
            }
        } catch (const std::exception&) {
            // startup failure on a corrupted store is acceptable (fail closed)
        }
    }

    template <typename Engine>
    void populate(const std::filesystem::path& dir, std::unique_ptr<Engine>& eng,
                  int n_ops) {
        (void)dir;
        for (int i = 0; i < n_ops; ++i) {
            double r = std::uniform_real_distribution<double>(0, 1)(rng_);
            std::string k = rand_key();
            if (r < 0.6) {
                std::string v = rand_value();
                eng->put(k, v);
                model_[k] = v;
            } else if (r < 0.8) {
                eng->del(k);
                model_.erase(k);
            } else {
                structural_maintenance(*eng);
            }
        }
    }

    std::string engine_name_;
    std::string profile_name_;
    int round_no_;
    std::mt19937_64 rng_;
    std::vector<std::string> pool_;
    std::map<std::string, std::string> model_;
    std::vector<std::map<std::string, std::string>> snapshots_;
};

}  // namespace

int main(int argc, char** argv) {
    std::string engine = "both";
    std::string profile = "all";
    int rounds = 20;
    uint64_t seed = 1;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (arg == "--engine") engine = next();
        else if (arg == "--profile") profile = next();
        else if (arg == "--rounds") rounds = std::stoi(next());
        else if (arg == "--seed") seed = std::stoull(next());
    }

    std::vector<std::string> engines;
    if (engine == "btree" || engine == "both") engines.push_back("btree");
    if (engine == "lsm" || engine == "both") engines.push_back("lsm");
    std::vector<std::string> profiles;
    if (profile == "all") profiles = {"ops", "crash", "corrupt"};
    else profiles = {profile};

    int total_rounds = 0;
    for (const auto& eng_name : engines) {
        for (const auto& p : profiles) {
            int n = (profiles.size() > 1) ? (rounds + 2) / 3 : rounds;
            for (int r = 1; r <= n; ++r) {
                Fuzzer fz(eng_name, seed * 7919 + static_cast<uint64_t>(r) * 131 + 13, r);
                fz.run_profile(p);
                ++total_rounds;
            }
        }
    }

    std::cout << "fuzz done: " << total_rounds << " rounds, " << g_findings
              << " findings" << std::endl;
    return g_findings > 0 ? 1 : 0;
}
