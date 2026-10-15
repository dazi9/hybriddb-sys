#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <random>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hybriddb/bloom.hpp"
#include "hybriddb/page.hpp"
#include "hybriddb/wal.hpp"

namespace hybriddb {

struct MemTableEntry {
    std::string key;
    std::string value;
    lsn_t lsn{0};
    bool is_tombstone{false};
};

/**
 * Probabilistic SkipList MemTable (max level 12, branching probability p = 0.25).
 * Provides O(log N) lock-free-friendly ordered insertion, point lookup, and range scan.
 */
class SkipListMemTable {
public:
    static constexpr int MAX_LEVEL = 12;

    explicit SkipListMemTable(uint32_t seed = 0xC0FFEE01U);
    ~SkipListMemTable();

    SkipListMemTable(const SkipListMemTable&) = delete;
    SkipListMemTable& operator=(const SkipListMemTable&) = delete;

    void upsert(std::string_view key, std::string_view value, lsn_t lsn, bool is_tombstone);
    bool get(std::string_view key, std::string* out_value, bool* out_tombstone) const;
    [[nodiscard]] std::vector<MemTableEntry> export_sorted() const;
    void clear();

    [[nodiscard]] size_t size() const noexcept { return count_; }
    [[nodiscard]] size_t approximate_bytes() const noexcept { return approx_bytes_; }

private:
    struct Node {
        MemTableEntry entry;
        int height{1};
        std::array<Node*, MAX_LEVEL> next{};
    };

    int random_height();

    Node* head_{nullptr};
    int current_max_height_{1};
    size_t count_{0};
    size_t approx_bytes_{0};
    std::mt19937 rng_;
};

struct FencePointer {
    std::string min_key;
    std::string max_key;
    page_id_t page_id{0};
};

enum class BloomTuningMode {
    UNIFORM = 0,         // State-of-the-art fixed bits-per-key across all levels
    MONKEY_OPTIMAL = 1   // Dayan et al. (SIGMOD '17) level-proportional bits-per-key
};

struct SSTableMeta {
    uint64_t sst_id{0};
    uint32_t level{0};
    std::filesystem::path file_path{};
    size_t num_entries{0};
    uint32_t num_data_pages{0};
    std::string min_key{};
    std::string max_key{};
    std::vector<FencePointer> fence_pointers{};
    BloomFilter bloom{};
    std::vector<std::string> cached_keys_for_retune{};
};

struct LSMConfig {
    std::filesystem::path db_dir{"lsm_data"};
    size_t memtable_max_bytes{32 * 1024};  // 32 KiB default for fast multi-level testing
    size_t l0_compaction_trigger{4};
    size_t max_levels{3};
    size_t level_size_ratio{4};
    double bloom_bits_per_key{8.0};
    BloomTuningMode bloom_mode{BloomTuningMode::MONKEY_OPTIMAL};
    bool sync_wal_on_write{false};
    bool verify_crc{true};
};

struct LSMLevelSummary {
    uint32_t level{0};
    size_t num_sstables{0};
    size_t total_entries{0};
    double bits_per_key{0.0};
    size_t filter_bytes{0};
};

/**
 * Multi-Level Log-Structured Merge-Tree (LSM-Tree) Storage Engine
 * with SkipList MemTable, 4KB-page SSTables, Fence Pointers, Monkey/Uniform Bloom Filters,
 * Multi-Way Merge Compaction, and WAL Crash Recovery.
 */
class LSMTreeEngine {
public:
    explicit LSMTreeEngine(const LSMConfig& config);
    ~LSMTreeEngine();

    LSMTreeEngine(const LSMTreeEngine&) = delete;
    LSMTreeEngine& operator=(const LSMTreeEngine&) = delete;

    Status put(std::string_view key, std::string_view value);
    Status get(std::string_view key, std::string* out_value);
    Status del(std::string_view key);
    Status scan(
        std::string_view start_key,
        std::string_view end_key,
        std::vector<std::pair<std::string, std::string>>* out_results,
        size_t limit = 0);

    Status flush_memtable();
    Status compact_level(uint32_t level);
    Status compact_all();
    void retune_bloom_filters(BloomTuningMode mode, double avg_bits_per_key);
    void simulate_crash_without_flush();

    [[nodiscard]] const StorageStats& stats() const noexcept { return stats_; }
    void reset_read_stats() noexcept {
        stats_.logical_reads = 0;
        stats_.physical_page_reads = 0;
        stats_.bloom_checks = 0;
        stats_.bloom_true_negatives = 0;
        stats_.bloom_false_positives = 0;
    }

    [[nodiscard]] std::vector<LSMLevelSummary> inspect_levels() const;
    [[nodiscard]] uint64_t disk_footprint_bytes() const noexcept;
    [[nodiscard]] size_t total_bloom_filter_bytes() const noexcept;

private:
    Status initialize_or_recover();
    Status write_sstable_from_entries(
        uint32_t level,
        const std::vector<MemTableEntry>& entries,
        double bits_per_key,
        SSTableMeta* out_meta);
    Status read_entries_from_sstable(
        const SSTableMeta& sst,
        std::vector<MemTableEntry>* out_entries);
    Status search_in_sstable(
        const SSTableMeta& sst,
        std::string_view key,
        std::string* out_value,
        bool* out_tombstone,
        bool* out_found);
    Status maybe_trigger_compaction();
    void apply_bloom_tuning_internal();
    Status save_manifest();
    Status load_manifest();

    LSMConfig config_;
    StorageStats stats_{};
    std::unique_ptr<SkipListMemTable> memtable_;
    std::unique_ptr<WALManager> wal_;
    std::vector<std::vector<SSTableMeta>> levels_;
    uint64_t next_sst_id_{1};
    bool crashed_simulation_{false};
    mutable std::shared_mutex rw_latch_;
};

}  // namespace hybriddb
