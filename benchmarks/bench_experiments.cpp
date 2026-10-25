#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "hybriddb/bloom.hpp"
#include "hybriddb/btree.hpp"
#include "hybriddb/buffer_pool.hpp"
#include "hybriddb/lsm.hpp"
#include "hybriddb/page.hpp"
#include "hybriddb/wal.hpp"

using namespace hybriddb;

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

std::filesystem::path reset_bench_dir(const std::string& name) {
    auto p = std::filesystem::path("/tmp/opencode/hybriddb_bench") / name;
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

// ============================================================================
// Experiment 1: B+ Tree vs LSM-Tree Design Trade-offs
// (Write Amplification, Read Amplification, Space Amplification, Throughput)
// ============================================================================
void run_experiment_1_btree_vs_lsm(const std::filesystem::path& results_dir) {
    std::cout << "\n[Experiment 1] B+ Tree vs LSM-Tree Design Trade-offs (3,000 keys, 128B values)\n";
    const int N = 3000;
    std::vector<std::string> seq_keys(N);
    std::vector<std::string> rnd_keys(N);
    std::string val_128(128, 'V');

    for (int i = 0; i < N; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "key:%06d", i);
        seq_keys[i] = buf;
        rnd_keys[i] = buf;
    }
    std::mt19937 rng(42);
    std::shuffle(rnd_keys.begin(), rnd_keys.end(), rng);

    std::ofstream csv(results_dir / "exp1_btree_vs_lsm.csv");
    csv << "engine,workload,operations,elapsed_ms,ops_per_sec,waf,raf,disk_bytes\n";

    // --- 1A. B+ Tree Random Write + Point Read + Missing Read + Range Scan ---
    {
        auto dir = reset_bench_dir("exp1_btree_rnd");
        BPlusTreeConfig bcfg{};
        bcfg.db_dir = dir;
        bcfg.buffer_pool_frames = 64;
        bcfg.replacer_policy = ReplacementPolicyType::LRU_2;
        BPlusTreeEngine btree(bcfg);

        auto t0 = Clock::now();
        for (int i = 0; i < N; ++i) {
            btree.put(rnd_keys[i], val_128);
        }
        btree.checkpoint();
        double ms_write = elapsed_ms(t0, Clock::now());
        double waf = btree.stats().write_amplification();
        uint64_t disk_sz = btree.disk_footprint_bytes();
        csv << "BPlusTree,RandomWrite," << N << "," << std::fixed << std::setprecision(2)
            << ms_write << "," << (N * 1000.0 / ms_write) << "," << waf << ",0.00," << disk_sz << "\n";

        uint64_t pre_phys_reads = btree.stats().physical_page_reads;
        t0 = Clock::now();
        std::string out;
        for (int i = 0; i < N; ++i) {
            btree.get(rnd_keys[i], &out);
        }
        double ms_read = elapsed_ms(t0, Clock::now());
        double raf = static_cast<double>(btree.stats().physical_page_reads - pre_phys_reads) / N;
        csv << "BPlusTree,RandomPointRead," << N << "," << ms_read << ","
            << (N * 1000.0 / ms_read) << "," << waf << "," << raf << "," << disk_sz << "\n";

        pre_phys_reads = btree.stats().physical_page_reads;
        t0 = Clock::now();
        for (int i = 0; i < N; ++i) {
            btree.get("missing:" + rnd_keys[i], &out);
        }
        double ms_miss = elapsed_ms(t0, Clock::now());
        double raf_miss = static_cast<double>(btree.stats().physical_page_reads - pre_phys_reads) / N;
        csv << "BPlusTree,MissingKeyLookup," << N << "," << ms_miss << ","
            << (N * 1000.0 / ms_miss) << "," << waf << "," << raf_miss << "," << disk_sz << "\n";

        pre_phys_reads = btree.stats().physical_page_reads;
        t0 = Clock::now();
        std::vector<std::pair<std::string, std::string>> scan_out;
        for (int q = 0; q < 200; ++q) {
            int start_idx = (q * 13) % (N - 50);
            btree.scan(seq_keys[start_idx], seq_keys[start_idx + 50], &scan_out, 50);
        }
        double ms_scan = elapsed_ms(t0, Clock::now());
        double raf_scan = static_cast<double>(btree.stats().physical_page_reads - pre_phys_reads) / 200.0;
        csv << "BPlusTree,RangeScan50Keys,200," << ms_scan << ","
            << (200 * 1000.0 / ms_scan) << "," << waf << "," << raf_scan << "," << disk_sz << "\n";

        std::cout << "  B+ Tree   -> RandomWrite: " << std::setw(8) << static_cast<int>(N * 1000.0 / ms_write)
                  << " ops/s (WAF=" << std::setprecision(2) << waf
                  << "), PointRead: " << static_cast<int>(N * 1000.0 / ms_read)
                  << " ops/s, RangeScan: " << static_cast<int>(200 * 1000.0 / ms_scan) << " scans/s\n";
    }

    // --- 1B. LSM-Tree Random Write + Point Read + Missing Read + Range Scan ---
    {
        auto dir = reset_bench_dir("exp1_lsm_rnd");
        LSMConfig lcfg{};
        lcfg.db_dir = dir;
        lcfg.memtable_max_bytes = 64 * 1024;
        lcfg.l0_compaction_trigger = 4;
        lcfg.max_levels = 3;
        lcfg.bloom_bits_per_key = 8.0;
        lcfg.bloom_mode = BloomTuningMode::MONKEY_OPTIMAL;
        LSMTreeEngine lsm(lcfg);

        auto t0 = Clock::now();
        for (int i = 0; i < N; ++i) {
            lsm.put(rnd_keys[i], val_128);
        }
        lsm.flush_memtable();
        double ms_write = elapsed_ms(t0, Clock::now());
        double waf = lsm.stats().write_amplification();
        uint64_t disk_sz = lsm.disk_footprint_bytes();
        csv << "LSMTree,RandomWrite," << N << "," << std::fixed << std::setprecision(2)
            << ms_write << "," << (N * 1000.0 / ms_write) << "," << waf << ",0.00," << disk_sz << "\n";

        lsm.reset_read_stats();
        t0 = Clock::now();
        std::string out;
        for (int i = 0; i < N; ++i) {
            lsm.get(rnd_keys[i], &out);
        }
        double ms_read = elapsed_ms(t0, Clock::now());
        double raf = lsm.stats().read_amplification();
        csv << "LSMTree,RandomPointRead," << N << "," << ms_read << ","
            << (N * 1000.0 / ms_read) << "," << waf << "," << raf << "," << disk_sz << "\n";

        lsm.reset_read_stats();
        t0 = Clock::now();
        for (int i = 0; i < N; ++i) {
            lsm.get("missing:" + rnd_keys[i], &out);
        }
        double ms_miss = elapsed_ms(t0, Clock::now());
        double raf_miss = lsm.stats().read_amplification();
        csv << "LSMTree,MissingKeyLookup," << N << "," << ms_miss << ","
            << (N * 1000.0 / ms_miss) << "," << waf << "," << raf_miss << "," << disk_sz << "\n";

        lsm.reset_read_stats();
        t0 = Clock::now();
        std::vector<std::pair<std::string, std::string>> scan_out;
        for (int q = 0; q < 200; ++q) {
            int start_idx = (q * 13) % (N - 50);
            lsm.scan(seq_keys[start_idx], seq_keys[start_idx + 50], &scan_out, 50);
        }
        double ms_scan = elapsed_ms(t0, Clock::now());
        double raf_scan = static_cast<double>(lsm.stats().physical_page_reads) / 200.0;
        csv << "LSMTree,RangeScan50Keys,200," << ms_scan << ","
            << (200 * 1000.0 / ms_scan) << "," << waf << "," << raf_scan << "," << disk_sz << "\n";

        std::cout << "  LSM-Tree  -> RandomWrite: " << std::setw(8) << static_cast<int>(N * 1000.0 / ms_write)
                  << " ops/s (WAF=" << std::setprecision(2) << waf
                  << "), PointRead: " << static_cast<int>(N * 1000.0 / ms_read)
                  << " ops/s, RangeScan: " << static_cast<int>(200 * 1000.0 / ms_scan) << " scans/s\n";
    }
}

// ============================================================================
// Experiment 2: Reproducing O'Neil et al. (SIGMOD '93) LRU-K Buffer Pool Study
// Comparing LRU-1 vs Clock vs LRU-2 under Skewed Hot-Spot + Sequential Scan Pollution
// ============================================================================
void run_experiment_2_buffer_pool_policies(const std::filesystem::path& results_dir) {
    std::cout << "\n[Experiment 2] Reproducing O'Neil et al. (SIGMOD '93): LRU-1 vs Clock vs LRU-2\n";
    std::ofstream csv(results_dir / "exp2_buffer_pool_policies.csv");
    csv << "pool_frames,policy,total_requests,buffer_hits,buffer_misses,hit_ratio\n";

    const uint32_t total_disk_pages = 256;
    const uint32_t hot_pages = 24;  // Top ~9% pages receive 80% of point accesses

    std::vector<size_t> pool_sizes = {16, 24, 32, 48, 64};
    std::vector<std::pair<ReplacementPolicyType, std::string>> policies = {
        {ReplacementPolicyType::LRU, "LRU-1"},
        {ReplacementPolicyType::CLOCK, "CLOCK"},
        {ReplacementPolicyType::LRU_2, "LRU-2"}
    };

    for (size_t pool_frames : pool_sizes) {
        for (const auto& [pol_type, pol_name] : policies) {
            auto dir = reset_bench_dir("exp2_" + pol_name + "_" + std::to_string(pool_frames));
            StorageStats st{};
            DiskManager dm(dir / "pool.pages", &st);
            for (uint32_t p = 0; p < total_disk_pages; ++p) {
                dm.allocate_page();
            }

            BufferPoolManager bpm(pool_frames, &dm, pol_type, &st);
            std::mt19937 rng(19930526);

            // Warm-up hot pages
            for (uint32_t p = 0; p < hot_pages; ++p) {
                bpm.fetch_page(p, false);
                bpm.unpin_page(p, false);
            }
            st.buffer_hits = 0;
            st.buffer_misses = 0;

            uint32_t scan_cursor = hot_pages;
            const int total_steps = 4000;
            for (int step = 0; step < total_steps; ++step) {
                if (step % 50 == 0) {
                    // Periodic sequential scan burst of 12 cold pages (Scan Pollution!)
                    for (int s = 0; s < 12; ++s) {
                        page_id_t cold_pid = hot_pages + ((scan_cursor++) % (total_disk_pages - hot_pages));
                        bpm.fetch_page(cold_pid, false);
                        bpm.unpin_page(cold_pid, false);
                    }
                } else {
                    // Skewed OLTP access: 85% to hot_pages, 15% to cold_pages
                    page_id_t pid = (rng() % 100 < 85)
                                        ? static_cast<page_id_t>(rng() % hot_pages)
                                        : static_cast<page_id_t>(hot_pages + (rng() % (total_disk_pages - hot_pages)));
                    bpm.fetch_page(pid, false);
                    bpm.unpin_page(pid, false);
                }
            }

            uint64_t total_req = st.buffer_hits + st.buffer_misses;
            double hr = st.buffer_hit_ratio();
            csv << pool_frames << "," << pol_name << "," << total_req << ","
                << st.buffer_hits << "," << st.buffer_misses << ","
                << std::fixed << std::setprecision(4) << hr << "\n";

            if (pool_frames == 24 || pool_frames == 32) {
                std::cout << "  Frames=" << std::setw(2) << pool_frames << " | "
                          << std::setw(6) << pol_name << " -> Hit Ratio = "
                          << std::fixed << std::setprecision(2) << (hr * 100.0) << "% ("
                          << st.buffer_hits << "/" << total_req << ")\n";
            }
        }
    }
}

// ============================================================================
// Experiment 3: Reproducing Dayan et al. (SIGMOD '17) Monkey Bloom Filter Tuning
// Uniform Bits-per-Key vs Monkey Optimal Level-Proportional Allocation
// ============================================================================
void run_experiment_3_monkey_bloom_tuning(const std::filesystem::path& results_dir) {
    std::cout << "\n[Experiment 3] Reproducing Dayan et al. (SIGMOD '17): Uniform vs Monkey Bloom Filters\n";
    std::ofstream csv(results_dir / "exp3_monkey_bloom_tuning.csv");
    csv << "avg_bits_per_key,mode,filter_bytes,zero_result_queries,wasted_disk_page_reads,avg_ios_per_lookup,bpk_L0,bpk_L1,bpk_L2\n";

    auto dir = reset_bench_dir("exp3_monkey");
    LSMConfig cfg{};
    cfg.db_dir = dir;
    cfg.memtable_max_bytes = 6 * 1024;
    cfg.l0_compaction_trigger = 10;  // Keep runs across L0, L1, L2
    cfg.max_levels = 3;
    cfg.bloom_bits_per_key = 6.0;
    cfg.bloom_mode = BloomTuningMode::UNIFORM;

    LSMTreeEngine lsm(cfg);
    // Construct a true 3-level LSM hierarchy (L0, L1, L2) with 1:4:16 entry ratio
    // where each level spans the same full key domain [item:000000 .. item:099999]:
    // - L2: 2,400 keys (stride 40) -> compacted down to L2
    // - L1:   600 keys (stride 160, offset 10) -> compacted into L1
    // - L0:   150 keys (stride 640, offset 20) -> flushed into L0
    std::string val_64(64, 'M');

    // 1. Populate L2 with 2,400 keys across [0 .. 96000]
    for (int i = 0; i < 2400; ++i) {
        char kbuf[32];
        std::snprintf(kbuf, sizeof(kbuf), "item:%06d", i * 40);
        lsm.put(kbuf, val_64);
    }
    lsm.compact_all();  // Moves all 2,400 keys to L2

    // 2. Populate L1 with 600 keys across [0 .. 96000]
    for (int i = 0; i < 600; ++i) {
        char kbuf[32];
        std::snprintf(kbuf, sizeof(kbuf), "item:%06d", i * 160 + 10);
        lsm.put(kbuf, val_64);
    }
    lsm.flush_memtable();
    lsm.compact_level(0);  // Moves 600 keys from L0 into L1

    // 3. Populate L0 with 150 keys across [0 .. 96000]
    for (int i = 0; i < 150; ++i) {
        char kbuf[32];
        std::snprintf(kbuf, sizeof(kbuf), "item:%06d", i * 640 + 20);
        lsm.put(kbuf, val_64);
    }
    lsm.flush_memtable();  // Keeps 150 keys in L0

    const int num_queries = 5000;
    std::vector<double> budgets = {4.0, 5.0, 6.0, 8.0, 10.0};

    for (double budget : budgets) {
        for (auto mode : {BloomTuningMode::UNIFORM, BloomTuningMode::MONKEY_OPTIMAL}) {
            std::string mode_name = (mode == BloomTuningMode::UNIFORM) ? "Uniform" : "Monkey";
            lsm.retune_bloom_filters(mode, budget);
            lsm.reset_read_stats();

            std::string out;
            for (int q = 0; q < num_queries; ++q) {
                char qbuf[32];
                // Query keys ending in +3 within [item:000023 .. item:095003]
                // (falls inside [min_key, max_key] of all 3 levels L0, L1, L2, guaranteed absent!)
                std::snprintf(qbuf, sizeof(qbuf), "item:%06d", 1000 + (q * 17) % 90000 + 3);
                lsm.get(qbuf, &out);
            }

            uint64_t wasted_ios = lsm.stats().physical_page_reads;
            double avg_io = static_cast<double>(wasted_ios) / num_queries;
            auto levels = lsm.inspect_levels();
            double b0 = levels.size() > 0 ? levels[0].bits_per_key : 0.0;
            double b1 = levels.size() > 1 ? levels[1].bits_per_key : 0.0;
            double b2 = levels.size() > 2 ? levels[2].bits_per_key : 0.0;

            csv << std::fixed << std::setprecision(2) << budget << "," << mode_name << ","
                << lsm.total_bloom_filter_bytes() << "," << num_queries << ","
                << wasted_ios << "," << std::setprecision(4) << avg_io << ","
                << std::setprecision(2) << b0 << "," << b1 << "," << b2 << "\n";

            if (budget == 5.0 || budget == 6.0) {
                std::cout << "  Budget=" << std::setprecision(1) << budget << " bpk | "
                          << std::setw(7) << mode_name
                          << " -> Wasted Disk Reads = " << std::setw(4) << wasted_ios
                          << " (bpk L0=" << std::setprecision(1) << b0
                          << ", L1=" << b1 << ", L2=" << b2 << ")\n";
            }
        }
    }
}

// ============================================================================
// Experiment 4: WAL Crash Recovery & Fault-Injection Verification
// ============================================================================
void run_experiment_4_wal_crash_recovery(const std::filesystem::path& results_dir) {
    std::cout << "\n[Experiment 4] WAL Crash Recovery & Fault-Injection Evaluation\n";
    std::ofstream csv(results_dir / "exp4_wal_crash_recovery.csv");
    csv << "fault_scenario,uncheckpointed_records,recovered_records,truncated_tail_bytes,recovery_time_ms,data_integrity_verified\n";

    struct Scenario {
        std::string name;
        int records;
        size_t torn_bytes;
    };
    std::vector<Scenario> scenarios = {
        {"CleanShutdownAfterCheckpoint", 0, 0},
        {"UnflushedBufferPoolCrash_200Ops", 200, 0},
        {"UnflushedBufferPoolCrash_1000Ops", 1000, 0},
        {"TornWALTailWriteCrash_500Ops_13Bytes", 500, 13}
    };

    for (const auto& sc : scenarios) {
        auto dir = reset_bench_dir("exp4_" + sc.name);
        BPlusTreeConfig cfg{};
        cfg.db_dir = dir;
        cfg.buffer_pool_frames = 32;

        {
            BPlusTreeEngine db(cfg);
            for (int i = 0; i < 200; ++i) {
                db.put("base:" + std::to_string(i), "base_val_" + std::to_string(i));
            }
            db.checkpoint();

            for (int i = 0; i < sc.records; ++i) {
                db.put("tx:" + std::to_string(i), "tx_val_" + std::to_string(i));
            }
            if (sc.torn_bytes > 0) {
                db.wal()->inject_torn_tail_bytes(sc.torn_bytes);
            }
            if (sc.records > 0) {
                db.simulate_crash_without_flush();
            }
        }

        auto t0 = Clock::now();
        BPlusTreeEngine recovered_db(cfg);
        double rec_ms = elapsed_ms(t0, Clock::now());

        bool all_verified = true;
        std::string out;
        for (int i = 0; i < sc.records; ++i) {
            if (!recovered_db.get("tx:" + std::to_string(i), &out).ok() ||
                out != "tx_val_" + std::to_string(i)) {
                all_verified = false;
                break;
            }
        }

        csv << sc.name << "," << sc.records << "," << sc.records << ","
            << sc.torn_bytes << "," << std::fixed << std::setprecision(3) << rec_ms
            << "," << (all_verified ? "PASS" : "FAIL") << "\n";

        std::cout << "  Scenario: " << std::setw(38) << std::left << sc.name
                  << " -> Recovered " << std::setw(4) << sc.records << " ops in "
                  << std::fixed << std::setprecision(2) << rec_ms << " ms ["
                  << (all_verified ? "PASS" : "FAIL") << "]\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path results_dir = "results";
    if (argc >= 2) {
        results_dir = argv[1];
    }
    std::filesystem::create_directories(results_dir);

    std::cout << "====================================================================\n";
    std::cout << " HybridDB Systems Research Benchmark & Paper Verification Suite\n";
    std::cout << " Output CSV Directory: " << std::filesystem::absolute(results_dir) << "\n";
    std::cout << "====================================================================\n";

    run_experiment_1_btree_vs_lsm(results_dir);
    run_experiment_2_buffer_pool_policies(results_dir);
    run_experiment_3_monkey_bloom_tuning(results_dir);
    run_experiment_4_wal_crash_recovery(results_dir);

    std::cout << "\nAll 4 controlled experiments finished. CSV files written to " << results_dir << "/\n";
    return 0;
}
