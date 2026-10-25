#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "hybriddb/bloom.hpp"
#include "hybriddb/btree.hpp"
#include "hybriddb/buffer_pool.hpp"
#include "hybriddb/lsm.hpp"
#include "hybriddb/page.hpp"
#include "hybriddb/wal.hpp"

using namespace hybriddb;

namespace {

int g_tests_passed = 0;

#define RUN_TEST(fn)                                                    \
    do {                                                                \
        std::cout << "[ RUN      ] " << #fn << std::endl;               \
        fn();                                                           \
        ++g_tests_passed;                                               \
        std::cout << "[       OK ] " << #fn << std::endl;               \
    } while (0)

std::filesystem::path make_temp_dir(const std::string& name) {
    auto base = std::filesystem::path("/tmp/opencode/hybriddb_tests") / name;
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base, ec);
    return base;
}

void test_slotted_page_and_crc32() {
    SlottedPage page;
    page.reset(7, PageType::BTREE_LEAF, 42);

    assert(page.upsert_sorted("k30", "value_30").ok());
    assert(page.upsert_sorted("k10", "value_10").ok());
    assert(page.upsert_sorted("k20", "value_20").ok());
    assert(page.header()->num_slots == 3);

    // Slots must be maintained in sorted key order
    assert(page.key_at(0) == "k10");
    assert(page.key_at(1) == "k20");
    assert(page.key_at(2) == "k30");

    // Overwrite k20 with a longer value and verify in-place update + defragmentation
    assert(page.upsert_sorted("k20", "updated_longer_value_20").ok());
    auto [found, idx] = page.lower_bound_slot("k20");
    assert(found && idx == 1);
    assert(page.value_at(1) == "updated_longer_value_20");

    // Erase k10
    assert(page.erase_slot("k10"));
    assert(!page.erase_slot("k10"));
    assert(page.header()->num_slots == 2);

    // Seal checksum and verify bit-flip detection
    page.seal_checksum();
    assert(page.verify_checksum());
    page.raw_data()[128] ^= 0x01;
    assert(!page.verify_checksum());
}

void test_buffer_pool_lru2_scan_resistance() {
    auto dir = make_temp_dir("bpm_lru2");
    StorageStats stats_lru1{};
    StorageStats stats_lru2{};

    DiskManager dm1(dir / "d1.pages", &stats_lru1);
    DiskManager dm2(dir / "d2.pages", &stats_lru2);
    for (int i = 0; i < 32; ++i) {
        dm1.allocate_page();
        dm2.allocate_page();
    }

    // Pool of 8 frames: pages 0..5 are hot (accessed twice), then a sequential scan of pages 6..25
    // sweeps through the buffer pool.
    BufferPoolManager bpm_lru1(8, &dm1, ReplacementPolicyType::LRU, &stats_lru1);
    BufferPoolManager bpm_lru2(8, &dm2, ReplacementPolicyType::LRU_2, &stats_lru2);

    for (int round = 0; round < 2; ++round) {
        for (page_id_t pid = 0; pid < 6; ++pid) {
            assert(bpm_lru1.fetch_page(pid) != nullptr);
            bpm_lru1.unpin_page(pid, false);
            assert(bpm_lru2.fetch_page(pid) != nullptr);
            bpm_lru2.unpin_page(pid, false);
        }
    }

    // One-time sequential scan of pages 6..20 (each accessed only once, so backward 2-distance = +inf)
    for (page_id_t pid = 6; pid <= 20; ++pid) {
        assert(bpm_lru1.fetch_page(pid) != nullptr);
        bpm_lru1.unpin_page(pid, false);
        assert(bpm_lru2.fetch_page(pid) != nullptr);
        bpm_lru2.unpin_page(pid, false);
    }

    uint64_t hits_before_1 = stats_lru1.buffer_hits;
    uint64_t hits_before_2 = stats_lru2.buffer_hits;

    // Re-access hot pages 0..5: LRU-1 evicted all of them during the scan (0 hits),
    // whereas LRU-2 retained all 6 hot pages (6 hits)!
    for (page_id_t pid = 0; pid < 6; ++pid) {
        bpm_lru1.fetch_page(pid);
        bpm_lru1.unpin_page(pid, false);
        bpm_lru2.fetch_page(pid);
        bpm_lru2.unpin_page(pid, false);
    }

    uint64_t hot_hits_lru1 = stats_lru1.buffer_hits - hits_before_1;
    uint64_t hot_hits_lru2 = stats_lru2.buffer_hits - hits_before_2;
    assert(hot_hits_lru1 == 0);
    assert(hot_hits_lru2 == 6);
}

void test_wal_recovery_and_fault_injection() {
    auto dir = make_temp_dir("wal_fault");
    auto wal_path = dir / "test.wal";

    {
        WALManager wal(wal_path, true);
        assert(wal.append(WALOpType::PUT, "k1", "v1").ok());
        assert(wal.append(WALOpType::PUT, "k2", "v2").ok());
        assert(wal.append(WALOpType::DEL, "k1", "").ok());
        // Inject 11 bytes of torn tail garbage simulating a power failure mid-write
        assert(wal.inject_torn_tail_bytes(11).ok());
    }

    {
        WALManager wal(wal_path, true);
        std::vector<WALRecord> records;
        size_t truncated = 0;
        // Strict mode without tail recovery must report corruption
        assert(wal.recover(&records, false).is_corruption());

        // With recover_torn_tail=true, it truncates the 11 garbage bytes and recovers all 3 valid records
        assert(wal.recover(&records, true, &truncated).ok());
        assert(truncated == 11);
        assert(records.size() == 3);
        assert(records[0].key == "k1" && records[0].value == "v1");
        assert(records[1].key == "k2" && records[1].value == "v2");
        assert(records[2].op == WALOpType::DEL && records[2].key == "k1");

        // Inject a bit-flip inside the first record (mid-file corruption)
        assert(wal.inject_bit_flip_at_offset(10).ok());
        assert(wal.recover(&records, true).is_corruption());
    }
}

void test_bloom_filter_and_monkey_allocation() {
    std::vector<std::string> keys;
    for (int i = 0; i < 1000; ++i) {
        keys.push_back("present_key_" + std::to_string(i));
    }

    BloomFilter bf(10.0);
    bf.build(keys);
    // Zero false negatives allowed
    for (const auto& k : keys) {
        assert(bf.may_contain(k));
    }

    int fp = 0;
    for (int i = 0; i < 10000; ++i) {
        if (bf.may_contain("absent_key_" + std::to_string(i))) {
            ++fp;
        }
    }
    double fpr = static_cast<double>(fp) / 10000.0;
    // Theoretical FPR at 10 bits/key is ~0.0082; verify < 0.025
    assert(fpr < 0.025);

    // Verify Monkey (SIGMOD '17) bit allocation across 3 levels (N0=200, N1=800, N2=3200)
    std::vector<size_t> counts = {200, 800, 3200};
    double budget_bpk = 6.0;
    auto bpk = BloomFilter::compute_monkey_bits_per_key(counts, budget_bpk);
    assert(bpk.size() == 3);
    // Shallower levels must receive strictly more bits per key than deeper levels
    assert(bpk[0] > bpk[1]);
    assert(bpk[1] > bpk[2]);

    double total_bits = 0.0;
    size_t total_n = 0;
    for (size_t i = 0; i < counts.size(); ++i) {
        total_bits += static_cast<double>(counts[i]) * bpk[i];
        total_n += counts[i];
    }
    double effective_avg = total_bits / static_cast<double>(total_n);
    assert(std::abs(effective_avg - budget_bpk) < 0.25);
}

void test_bplustree_splits_scans_and_crash_recovery() {
    auto dir = make_temp_dir("btree_engine");
    BPlusTreeConfig cfg{};
    cfg.db_dir = dir;
    cfg.buffer_pool_frames = 32;
    cfg.replacer_policy = ReplacementPolicyType::LRU_2;

    // 1. Insert 400 keys to trigger multiple leaf and internal node splits
    {
        BPlusTreeEngine btree(cfg);
        for (int i = 0; i < 400; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "user:%05d", i);
            std::string val = "payload_for_user_" + std::to_string(i) + "_" + std::string(40, 'A');
            assert(btree.put(key_buf, val).ok());
        }

        // Delete every 10th key
        for (int i = 0; i < 400; i += 10) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "user:%05d", i);
            assert(btree.del(key_buf).ok());
        }

        auto tree_info = btree.inspect_tree();
        assert(tree_info.height >= 2);
        assert(tree_info.leaf_pages >= 8);

        // Verify range scan [user:00010, user:00025]
        std::vector<std::pair<std::string, std::string>> scan_res;
        assert(btree.scan("user:00010", "user:00025", &scan_res).ok());
        // 16 keys in [10..25], minus 10 and 20 deleted = 14 keys
        assert(scan_res.size() == 14);
        assert(scan_res.front().first == "user:00011");
        assert(scan_res.back().first == "user:00025");

        // Flush checkpoint, then write 20 more keys and simulate a sudden crash before buffer flush!
        assert(btree.checkpoint().ok());
        for (int i = 400; i < 420; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "user:%05d", i);
            assert(btree.put(key_buf, "post_checkpoint_crash_val").ok());
        }
        btree.simulate_crash_without_flush();
    }

    // 2. Reopen B+ Tree and verify WAL recovery restored all 20 post-checkpoint keys!
    {
        BPlusTreeEngine btree_recovered(cfg);
        for (int i = 0; i < 420; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "user:%05d", i);
            std::string val;
            Status st = btree_recovered.get(key_buf, &val);
            if (i < 400 && (i % 10 == 0)) {
                assert(st.is_not_found());
            } else if (i < 400) {
                assert(st.ok());
            } else {
                assert(st.ok());
                assert(val == "post_checkpoint_crash_val");
            }
        }
    }
}

void test_wal_lsn_continuity_across_sessions() {
    // Regression: a fresh WALManager restarts its lsn counter at 1 while the
    // meta page persists a large checkpoint_lsn_ from the previous session;
    // recovery then skipped every record written after that checkpoint.
    // Session B's writes (post-checkpoint, crash-terminated) must survive.
    auto dir = make_temp_dir("wal_lsn_continuity");
    BPlusTreeConfig cfg{};
    cfg.db_dir = dir;
    cfg.buffer_pool_frames = 32;

    {
        // Session A: writes + clean checkpoint + clean close.
        BPlusTreeEngine btree(cfg);
        for (int i = 0; i < 100; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "user:%05d", i);
            assert(btree.put(key_buf, "session_a").ok());
        }
        assert(btree.checkpoint().ok());
    }

    {
        // Session B: fresh WALManager instance; more writes; hard crash
        // without flush (destructor must skip the final checkpoint).
        BPlusTreeEngine btree(cfg);
        for (int i = 100; i < 150; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "user:%05d", i);
            assert(btree.put(key_buf, "session_b").ok());
        }
        btree.simulate_crash_without_flush();
    }

    {
        // Session C: recovery must replay session B's WAL records even though
        // their lsns are numerically small relative to session A's watermark.
        BPlusTreeEngine btree(cfg);
        for (int i = 0; i < 150; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "user:%05d", i);
            std::string val;
            Status st = btree.get(key_buf, &val);
            assert(st.ok());
            assert(val == (i < 100 ? "session_a" : "session_b"));
        }
    }
}

void test_crc32_table_equivalence() {
    // Regression: crc32_ieee was rewritten from a bitwise loop to slice-by-8
    // tables; the value stream must be bit-identical or every on-disk CRC
    // (pages, WAL, manifest) would be invalidated.
    auto bitwise_reference = [](const uint8_t* data, size_t len, uint32_t prev) {
        uint32_t crc = ~prev;
        for (size_t i = 0; i < len; ++i) {
            crc ^= data[i];
            for (int bit = 0; bit < 8; ++bit) {
                uint32_t mask = -(crc & 1U);
                crc = (crc >> 1) ^ (0xEDB88320U & mask);
            }
        }
        return ~crc;
    };

    // Standard CRC-32/ISO-HDLC check value.
    assert(crc32_ieee("123456789", 9) == 0xCBF43926U);

    std::mt19937 rng(42);
    std::vector<uint8_t> buf(600);
    for (auto& b : buf) b = static_cast<uint8_t>(rng() & 0xFFU);
    for (size_t len = 0; len <= 500; ++len) {
        assert(crc32_ieee(buf.data(), len) == bitwise_reference(buf.data(), len, 0));
        if (len >= 8) {
            // chained calls must equal a single call over the concatenation
            uint32_t chained = crc32_ieee(buf.data(), 4);
            chained = crc32_ieee(buf.data() + 4, len - 4, chained);
            assert(chained == crc32_ieee(buf.data(), len));
        }
    }
}

void test_btree_crash_before_first_checkpoint() {
    // Regression: with no checkpoint ever run, the on-disk tree was just the
    // blank creation skeleton; recovery used to navigate garbage and lose
    // every write. WAL replay must rebuild from an empty root instead.
    auto dir = make_temp_dir("btree_crash_no_ckpt");
    BPlusTreeConfig cfg{};
    cfg.db_dir = dir;
    cfg.buffer_pool_frames = 32;
    {
        BPlusTreeEngine btree(cfg);
        for (int i = 0; i < 80; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "user:%05d", i);
            assert(btree.put(key_buf, "pre_ckpt_val").ok());
        }
        btree.simulate_crash_without_flush();
    }
    {
        BPlusTreeEngine btree(cfg);
        for (int i = 0; i < 80; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "user:%05d", i);
            std::string val;
            assert(btree.get(key_buf, &val).ok());
            assert(val == "pre_ckpt_val");
        }
    }
}

void test_btree_dirty_pressure_small_pool() {
    // Regression: dirty pages must never be written outside a checkpoint
    // (no-steal policy). A tiny pool forces pressure checkpoints mid-session;
    // a crash afterwards must still recover via the WAL.
    auto dir = make_temp_dir("btree_pressure");
    BPlusTreeConfig cfg{};
    cfg.db_dir = dir;
    cfg.buffer_pool_frames = 6;
    {
        BPlusTreeEngine btree(cfg);
        for (int i = 0; i < 150; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "user:%05d", i);
            assert(btree.put(key_buf, "payload_" + std::to_string(i)).ok());
        }
        for (int i = 0; i < 150; i += 7) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "user:%05d", i);
            assert(btree.del(key_buf).ok());
        }
        btree.simulate_crash_without_flush();
    }
    {
        BPlusTreeEngine btree(cfg);
        for (int i = 0; i < 150; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "user:%05d", i);
            std::string val;
            Status st = btree.get(key_buf, &val);
            if (i % 7 == 0) {
                assert(st.is_not_found());
            } else {
                assert(st.ok());
            }
        }
    }
}

void test_lsm_manifest_corruption_fails_closed() {
    // Regression: the MANIFEST is the source of truth for live SSTables.
    // A corrupted manifest used to load silently, dropping SSTs and serving
    // stale values; now the checksum must abort the open.
    auto dir = make_temp_dir("lsm_manifest_corrupt");
    LSMConfig cfg{};
    cfg.db_dir = dir;
    cfg.memtable_max_bytes = 8 * 1024;
    {
        LSMTreeEngine lsm(cfg);
        for (int i = 0; i < 100; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "k:%05d", i);
            assert(lsm.put(key_buf, "v" + std::to_string(i)).ok());
        }
        assert(lsm.compact_all().ok());
    }

    auto mpath = dir / "MANIFEST";
    {
        std::fstream f(mpath, std::ios::binary | std::ios::in | std::ios::out);
        assert(f.is_open());
        f.seekp(0, std::ios::end);
        auto size = f.tellp();
        f.seekp(size / 2);
        char byte;
        f.read(&byte, 1);
        f.seekp(size / 2);
        byte ^= 0x40;
        f.write(&byte, 1);
    }

    bool threw = false;
    try {
        LSMTreeEngine lsm2(cfg);
        (void)lsm2;
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
}

void test_lsm_sst_corruption_fails_closed() {
    // Regression: a bit flip inside an SST data page used to poison the
    // fence-pointer/bloom rebuild (break-on-error), letting the bloom
    // silently hide keys so older versions surfaced as fresh reads.
    // Now the open must fail closed.
    auto dir = make_temp_dir("lsm_sst_corrupt");
    LSMConfig cfg{};
    cfg.db_dir = dir;
    cfg.memtable_max_bytes = 8 * 1024;
    {
        LSMTreeEngine lsm(cfg);
        for (int i = 0; i < 100; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "k:%05d", i);
            assert(lsm.put(key_buf, "v" + std::to_string(i)).ok());
        }
        assert(lsm.compact_all().ok());
    }

    // Flip one bit in the middle of some SST data file.
    bool flipped = false;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const auto name = entry.path().filename().string();
        if (entry.is_regular_file() && name.rfind("sst_L", 0) == 0 &&
            entry.file_size() > 1024) {
            std::fstream f(entry.path(), std::ios::binary | std::ios::in | std::ios::out);
            assert(f.is_open());
            f.seekp(200);  // inside the first data page's record area
            char byte;
            f.read(&byte, 1);
            f.seekp(200);
            byte ^= 0x08;
            f.write(&byte, 1);
            flipped = true;
            break;
        }
    }
    assert(flipped);

    bool threw = false;
    try {
        LSMTreeEngine lsm2(cfg);
        (void)lsm2;
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
}

void test_lsm_compaction_monkey_and_crash_recovery() {
    auto dir = make_temp_dir("lsm_engine");
    LSMConfig cfg{};
    cfg.db_dir = dir;
    cfg.memtable_max_bytes = 8 * 1024;  // 8 KiB MemTable to trigger frequent L0 flushes & compactions
    cfg.l0_compaction_trigger = 3;
    cfg.max_levels = 3;
    cfg.bloom_bits_per_key = 8.0;
    cfg.bloom_mode = BloomTuningMode::MONKEY_OPTIMAL;

    {
        LSMTreeEngine lsm(cfg);
        // Write 500 keys across multiple versions
        for (int v = 0; v < 3; ++v) {
            for (int i = 0; i < 500; ++i) {
                char key_buf[32];
                std::snprintf(key_buf, sizeof(key_buf), "k:%05d", i);
                std::string val = "v" + std::to_string(v) + "_payload_" + std::to_string(i);
                assert(lsm.put(key_buf, val).ok());
            }
        }

        // Delete keys 0..49
        for (int i = 0; i < 50; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "k:%05d", i);
            assert(lsm.del(key_buf).ok());
        }

        // Write 10 un-flushed keys in MemTable and simulate crash!
        for (int i = 500; i < 510; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "k:%05d", i);
            assert(lsm.put(key_buf, "wal_recovered_lsm_val").ok());
        }
        lsm.simulate_crash_without_flush();
    }

    // Reopen LSM-Tree, verify WAL recovery + SSTable manifest recovery + full compaction
    {
        LSMTreeEngine lsm2(cfg);
        for (int i = 0; i < 50; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "k:%05d", i);
            std::string val;
            assert(lsm2.get(key_buf, &val).is_not_found());
        }
        for (int i = 50; i < 500; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "k:%05d", i);
            std::string val;
            assert(lsm2.get(key_buf, &val).ok());
            assert(val == "v2_payload_" + std::to_string(i));
        }
        for (int i = 500; i < 510; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "k:%05d", i);
            std::string val;
            assert(lsm2.get(key_buf, &val).ok());
            assert(val == "wal_recovered_lsm_val");
        }

        assert(lsm2.compact_all().ok());
        std::vector<std::pair<std::string, std::string>> scan_res;
        assert(lsm2.scan("k:00045", "k:00055", &scan_res).ok());
        // Keys 45..49 were deleted; keys 50..55 are live (6 keys)
        assert(scan_res.size() == 6);
        assert(scan_res.front().first == "k:00050");
        assert(scan_res.back().first == "k:00055");
    }
}

void test_concurrent_multithreaded_engines() {
    auto dir = make_temp_dir("concurrent");
    BPlusTreeConfig bcfg{};
    bcfg.db_dir = dir / "btree";
    bcfg.buffer_pool_frames = 64;

    LSMConfig lcfg{};
    lcfg.db_dir = dir / "lsm";
    lcfg.memtable_max_bytes = 16 * 1024;

    BPlusTreeEngine btree(bcfg);
    LSMTreeEngine lsm(lcfg);

    std::vector<std::thread> threads;
    for (int tid = 0; tid < 4; ++tid) {
        threads.emplace_back([tid, &btree, &lsm]() {
            for (int i = 0; i < 60; ++i) {
                std::string k = "t" + std::to_string(tid) + "_k" + std::to_string(i);
                std::string v = "val_" + std::to_string(tid) + "_" + std::to_string(i);
                assert(btree.put(k, v).ok());
                assert(lsm.put(k, v).ok());
                std::string out_b, out_l;
                assert(btree.get(k, &out_b).ok() && out_b == v);
                assert(lsm.get(k, &out_l).ok() && out_l == v);
            }
        });
    }
    for (auto& t : threads) t.join();
}

}  // namespace

int main() {
    std::cout << "==========================================================" << std::endl;
    std::cout << " HybridDB C++20 Storage Engine Automated Test Suite" << std::endl;
    std::cout << "==========================================================" << std::endl;

    RUN_TEST(test_slotted_page_and_crc32);
    RUN_TEST(test_crc32_table_equivalence);
    RUN_TEST(test_buffer_pool_lru2_scan_resistance);
    RUN_TEST(test_wal_recovery_and_fault_injection);
    RUN_TEST(test_bloom_filter_and_monkey_allocation);
    RUN_TEST(test_bplustree_splits_scans_and_crash_recovery);
    RUN_TEST(test_wal_lsn_continuity_across_sessions);
    RUN_TEST(test_btree_crash_before_first_checkpoint);
    RUN_TEST(test_btree_dirty_pressure_small_pool);
    RUN_TEST(test_lsm_manifest_corruption_fails_closed);
    RUN_TEST(test_lsm_sst_corruption_fails_closed);
    RUN_TEST(test_lsm_compaction_monkey_and_crash_recovery);
    RUN_TEST(test_concurrent_multithreaded_engines);

    std::cout << "----------------------------------------------------------" << std::endl;
    std::cout << " ALL " << g_tests_passed << " TEST SUITES PASSED SUCCESSFULLY!" << std::endl;
    std::cout << "----------------------------------------------------------" << std::endl;
    return 0;
}
