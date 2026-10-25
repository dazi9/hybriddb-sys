#pragma once

#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "hybriddb/page.hpp"

namespace hybriddb {

enum class ReplacementPolicyType {
    LRU = 0,
    CLOCK = 1,
    LRU_2 = 2
};

/**
 * Abstract interface for Buffer Pool frame replacement strategies.
 * Allows controlled-variable experiments comparing LRU-1, Clock, and O'Neil et al. (SIGMOD '93) LRU-2.
 */
class Replacer {
public:
    virtual ~Replacer() = default;
    virtual void record_access(frame_id_t frame_id, page_id_t page_id = INVALID_PAGE_ID) = 0;
    virtual void set_evictable(frame_id_t frame_id, bool evictable) = 0;
    virtual bool victim(frame_id_t* out_frame_id) = 0;
    virtual void remove(frame_id_t frame_id) = 0;
    [[nodiscard]] virtual size_t evictable_size() const = 0;
};

/**
 * Classical LRU-1 Replacer (vulnerable to sequential scan pollution).
 */
class LRUReplacer final : public Replacer {
public:
    explicit LRUReplacer(size_t num_frames);
    void record_access(frame_id_t frame_id, page_id_t page_id = INVALID_PAGE_ID) override;
    void set_evictable(frame_id_t frame_id, bool evictable) override;
    bool victim(frame_id_t* out_frame_id) override;
    void remove(frame_id_t frame_id) override;
    [[nodiscard]] size_t evictable_size() const override;

private:
    struct FrameNode {
        bool evictable{false};
        bool in_cache{false};
        std::list<frame_id_t>::iterator it{};
    };
    std::list<frame_id_t> lru_list_;  // front = MRU, back = LRU
    std::vector<FrameNode> nodes_;
    size_t evictable_count_{0};
};

/**
 * Clock (Second-Chance) Replacer.
 */
class ClockReplacer final : public Replacer {
public:
    explicit ClockReplacer(size_t num_frames);
    void record_access(frame_id_t frame_id, page_id_t page_id = INVALID_PAGE_ID) override;
    void set_evictable(frame_id_t frame_id, bool evictable) override;
    bool victim(frame_id_t* out_frame_id) override;
    void remove(frame_id_t frame_id) override;
    [[nodiscard]] size_t evictable_size() const override;

private:
    struct ClockSlot {
        bool present{false};
        bool evictable{false};
        bool ref_bit{false};
    };
    std::vector<ClockSlot> slots_;
    size_t hand_{0};
    size_t evictable_count_{0};
};

/**
 * O'Neil, O'Neil, Weikum (SIGMOD 1993) LRU-K (K=2) Replacer.
 *
 * Tracks the last K=2 reference timestamps per page_id (including a Retained Information
 * History for recently evicted pages).
 * - Pages with fewer than 2 accesses have Backward 2-distance = +infinity and are evicted
 *   first in FIFO/LRU-1 order (protecting hot pages from sequential scan pollution!).
 * - When all candidate pages have >= 2 accesses, evicts the page with the oldest
 *   2nd-most-recent access timestamp (maximum Backward 2-distance).
 */
class LRU2Replacer final : public Replacer {
public:
    explicit LRU2Replacer(size_t num_frames, size_t k = 2, size_t max_history_pages = 4096);
    void record_access(frame_id_t frame_id, page_id_t page_id = INVALID_PAGE_ID) override;
    void set_evictable(frame_id_t frame_id, bool evictable) override;
    bool victim(frame_id_t* out_frame_id) override;
    void remove(frame_id_t frame_id) override;
    [[nodiscard]] size_t evictable_size() const override;

private:
    struct FrameState {
        bool present{false};
        bool evictable{false};
        page_id_t page_id{INVALID_PAGE_ID};
    };
    size_t k_{2};
    size_t max_history_{4096};
    uint64_t current_timestamp_{0};
    std::vector<FrameState> frames_;
    std::unordered_map<page_id_t, std::deque<uint64_t>> history_;
    std::deque<page_id_t> history_fifo_;
    size_t evictable_count_{0};
};

/**
 * BufferPoolManager manages a fixed number of 4096-byte in-memory page frames.
 */
class BufferPoolManager {
public:
    BufferPoolManager(
        size_t pool_size_frames,
        DiskManager* disk_manager,
        ReplacementPolicyType policy = ReplacementPolicyType::LRU_2,
        StorageStats* stats = nullptr);
    ~BufferPoolManager();

    BufferPoolManager(const BufferPoolManager&) = delete;
    BufferPoolManager& operator=(const BufferPoolManager&) = delete;

    SlottedPage* fetch_page(page_id_t page_id, bool verify_crc = true);
    SlottedPage* new_page(page_id_t* out_page_id, PageType type);
    bool unpin_page(page_id_t page_id, bool is_dirty);
    Status flush_page(page_id_t page_id);
    Status flush_all_pages();

    // Simulates an abrupt power-loss crash by discarding all un-flushed dirty frames in RAM
    void simulate_crash_discard_dirty_frames();

    [[nodiscard]] size_t pool_size() const noexcept { return pool_size_; }

private:
    struct FrameMeta {
        page_id_t page_id{INVALID_PAGE_ID};
        uint32_t pin_count{0};
        bool is_dirty{false};
    };

    bool find_victim_frame(frame_id_t* out_frame);

    size_t pool_size_;
    DiskManager* disk_;
    StorageStats* stats_;
    std::unique_ptr<Replacer> replacer_;
    std::vector<SlottedPage> pages_;
    std::vector<FrameMeta> meta_;
    std::vector<frame_id_t> free_list_;
    std::unordered_map<page_id_t, frame_id_t> page_table_;
    mutable std::mutex latch_;
};

}  // namespace hybriddb
