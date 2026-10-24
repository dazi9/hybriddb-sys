#pragma once

#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hybriddb/buffer_pool.hpp"
#include "hybriddb/wal.hpp"

namespace hybriddb {

struct BPlusTreeConfig {
    std::filesystem::path db_dir{"btree_data"};
    size_t buffer_pool_frames{64};
    ReplacementPolicyType replacer_policy{ReplacementPolicyType::LRU_2};
    bool sync_wal_on_write{false};
    bool verify_crc{true};
};

struct BTreeStructureStats {
    uint32_t height{1};
    uint32_t internal_pages{0};
    uint32_t leaf_pages{0};
    uint64_t total_slots{0};
    double avg_leaf_fill_factor{0.0};
};

/**
 * Page-Managed B+ Tree Storage Engine with Buffer Pool & ARIES-style WAL Recovery.
 */
class BPlusTreeEngine {
public:
    explicit BPlusTreeEngine(const BPlusTreeConfig& config);
    ~BPlusTreeEngine();

    BPlusTreeEngine(const BPlusTreeEngine&) = delete;
    BPlusTreeEngine& operator=(const BPlusTreeEngine&) = delete;

    Status put(std::string_view key, std::string_view value);
    Status get(std::string_view key, std::string* out_value);
    Status del(std::string_view key);
    Status scan(
        std::string_view start_key,
        std::string_view end_key,
        std::vector<std::pair<std::string, std::string>>* out_results,
        size_t limit = 0);

    Status checkpoint();
    void simulate_crash_without_flush();
    WALManager* wal() noexcept { return wal_.get(); }
    BufferPoolManager* buffer_pool() noexcept { return bpm_.get(); }

    [[nodiscard]] const StorageStats& stats() const noexcept { return stats_; }
    [[nodiscard]] BTreeStructureStats inspect_tree();
    [[nodiscard]] uint64_t disk_footprint_bytes() const noexcept;

private:
    Status initialize_or_recover();
    Status write_meta_page(bool persist_immediately = false);
    Status read_meta_page();

    // Checkpoint variants: `_locked` assumes the caller already holds
    // rw_latch_ exclusively (used on the write path under dirty-frame
    // pressure); the public checkpoint() takes the lock itself.
    Status checkpoint_locked();
    void maybe_checkpoint_for_pressure_locked();

    Status apply_put_internal(std::string_view key, std::string_view value, lsn_t lsn);
    Status apply_del_internal(std::string_view key, lsn_t lsn);

    page_id_t find_leaf_page(
        std::string_view key,
        std::vector<page_id_t>* out_path = nullptr);
    Status insert_into_parent(
        const std::vector<page_id_t>& path,
        page_id_t left_child,
        std::string_view sep_key,
        page_id_t right_child,
        lsn_t lsn);

    static page_id_t decode_child_pid(std::string_view val) noexcept;
    static std::string encode_child_pid(page_id_t pid);

    BPlusTreeConfig config_;
    StorageStats stats_{};
    std::unique_ptr<DiskManager> disk_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<WALManager> wal_;

    page_id_t root_page_id_{INVALID_PAGE_ID};
    uint32_t tree_height_{1};
    lsn_t checkpoint_lsn_{0};
    bool crashed_simulation_{false};
    mutable std::shared_mutex rw_latch_;
};

}  // namespace hybriddb
