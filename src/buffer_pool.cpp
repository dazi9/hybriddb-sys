#include "hybriddb/buffer_pool.hpp"

#include <limits>

namespace hybriddb {

// ============================================================================
// 1. Classical LRU-1 Replacer
// ============================================================================
LRUReplacer::LRUReplacer(size_t num_frames) : nodes_(num_frames) {}

void LRUReplacer::record_access(frame_id_t frame_id, page_id_t /*page_id*/) {
    if (frame_id >= nodes_.size()) return;
    auto& node = nodes_[frame_id];
    if (node.in_cache) {
        lru_list_.erase(node.it);
    } else {
        node.in_cache = true;
    }
    lru_list_.push_front(frame_id);
    node.it = lru_list_.begin();
}

void LRUReplacer::set_evictable(frame_id_t frame_id, bool evictable) {
    if (frame_id >= nodes_.size()) return;
    auto& node = nodes_[frame_id];
    if (!node.in_cache) return;
    if (node.evictable && !evictable) {
        node.evictable = false;
        evictable_count_--;
    } else if (!node.evictable && evictable) {
        node.evictable = true;
        evictable_count_++;
    }
}

bool LRUReplacer::victim(frame_id_t* out_frame_id) {
    if (evictable_count_ == 0) return false;
    for (auto it = lru_list_.rbegin(); it != lru_list_.rend(); ++it) {
        frame_id_t fid = *it;
        if (nodes_[fid].evictable) {
            *out_frame_id = fid;
            remove(fid);
            return true;
        }
    }
    return false;
}

void LRUReplacer::remove(frame_id_t frame_id) {
    if (frame_id >= nodes_.size()) return;
    auto& node = nodes_[frame_id];
    if (!node.in_cache) return;
    if (node.evictable) {
        node.evictable = false;
        evictable_count_--;
    }
    lru_list_.erase(node.it);
    node.in_cache = false;
}

size_t LRUReplacer::evictable_size() const { return evictable_count_; }

// ============================================================================
// 2. Clock (Second-Chance) Replacer
// ============================================================================
ClockReplacer::ClockReplacer(size_t num_frames) : slots_(num_frames) {}

void ClockReplacer::record_access(frame_id_t frame_id, page_id_t /*page_id*/) {
    if (frame_id >= slots_.size()) return;
    slots_[frame_id].present = true;
    slots_[frame_id].ref_bit = true;
}

void ClockReplacer::set_evictable(frame_id_t frame_id, bool evictable) {
    if (frame_id >= slots_.size()) return;
    auto& s = slots_[frame_id];
    if (!s.present) return;
    if (s.evictable && !evictable) {
        s.evictable = false;
        evictable_count_--;
    } else if (!s.evictable && evictable) {
        s.evictable = true;
        evictable_count_++;
    }
}

bool ClockReplacer::victim(frame_id_t* out_frame_id) {
    if (evictable_count_ == 0 || slots_.empty()) return false;
    const size_t n = slots_.size();
    for (size_t scanned = 0; scanned <= 2 * n; ++scanned) {
        auto& s = slots_[hand_];
        frame_id_t curr = static_cast<frame_id_t>(hand_);
        hand_ = (hand_ + 1) % n;
        if (!s.present || !s.evictable) continue;
        if (s.ref_bit) {
            s.ref_bit = false;
        } else {
            *out_frame_id = curr;
            remove(curr);
            return true;
        }
    }
    return false;
}

void ClockReplacer::remove(frame_id_t frame_id) {
    if (frame_id >= slots_.size()) return;
    auto& s = slots_[frame_id];
    if (!s.present) return;
    if (s.evictable) {
        s.evictable = false;
        evictable_count_--;
    }
    s.present = false;
    s.ref_bit = false;
}

size_t ClockReplacer::evictable_size() const { return evictable_count_; }

// ============================================================================
// 3. O'Neil et al. (SIGMOD '93) LRU-K (K=2) Replacer with Retained History
// ============================================================================
LRU2Replacer::LRU2Replacer(size_t num_frames, size_t k, size_t max_history_pages)
    : k_(k < 1 ? 1 : k), max_history_(max_history_pages), frames_(num_frames) {}

void LRU2Replacer::record_access(frame_id_t frame_id, page_id_t page_id) {
    if (frame_id >= frames_.size()) return;
    ++current_timestamp_;
    auto& f = frames_[frame_id];
    f.present = true;
    page_id_t effective_pid = (page_id != INVALID_PAGE_ID) ? page_id : frame_id;
    f.page_id = effective_pid;

    auto it = history_.find(effective_pid);
    if (it == history_.end()) {
        if (history_.size() >= max_history_ && !history_fifo_.empty()) {
            page_id_t oldest_pid = history_fifo_.front();
            history_fifo_.pop_front();
            history_.erase(oldest_pid);
        }
        history_fifo_.push_back(effective_pid);
        it = history_.emplace(effective_pid, std::deque<uint64_t>{}).first;
    }
    auto& q = it->second;
    q.push_back(current_timestamp_);
    if (q.size() > k_) {
        q.pop_front();
    }
}

void LRU2Replacer::set_evictable(frame_id_t frame_id, bool evictable) {
    if (frame_id >= frames_.size()) return;
    auto& f = frames_[frame_id];
    if (!f.present) return;
    if (f.evictable && !evictable) {
        f.evictable = false;
        evictable_count_--;
    } else if (!f.evictable && evictable) {
        f.evictable = true;
        evictable_count_++;
    }
}

bool LRU2Replacer::victim(frame_id_t* out_frame_id) {
    if (evictable_count_ == 0) return false;

    frame_id_t best_inf_frame = INVALID_FRAME_ID;
    uint64_t oldest_inf_ts = std::numeric_limits<uint64_t>::max();

    frame_id_t best_k_frame = INVALID_FRAME_ID;
    uint64_t oldest_k_ts = std::numeric_limits<uint64_t>::max();

    for (frame_id_t fid = 0; fid < frames_.size(); ++fid) {
        const auto& f = frames_[fid];
        if (!f.present || !f.evictable) continue;
        auto it = history_.find(f.page_id);
        if (it == history_.end() || it->second.size() < k_) {
            uint64_t first_ts = (it != history_.end() && !it->second.empty())
                                    ? it->second.front()
                                    : 0;
            if (first_ts < oldest_inf_ts) {
                oldest_inf_ts = first_ts;
                best_inf_frame = fid;
            }
        } else {
            uint64_t k_ts = it->second.front();
            if (k_ts < oldest_k_ts) {
                oldest_k_ts = k_ts;
                best_k_frame = fid;
            }
        }
    }

    frame_id_t chosen = (best_inf_frame != INVALID_FRAME_ID) ? best_inf_frame : best_k_frame;
    if (chosen == INVALID_FRAME_ID) return false;
    *out_frame_id = chosen;
    remove(chosen);
    return true;
}

void LRU2Replacer::remove(frame_id_t frame_id) {
    if (frame_id >= frames_.size()) return;
    auto& f = frames_[frame_id];
    if (!f.present) return;
    if (f.evictable) {
        f.evictable = false;
        evictable_count_--;
    }
    f.present = false;
    // Notice: per O'Neil et al. SIGMOD '93 Section 2.1, we intentionally retain `history_[f.page_id]`
    // across evictions so correlated/frequent pages are recognized when re-referenced!
}

size_t LRU2Replacer::evictable_size() const { return evictable_count_; }

// ============================================================================
// 4. BufferPoolManager
// ============================================================================
BufferPoolManager::BufferPoolManager(
    size_t pool_size_frames,
    DiskManager* disk_manager,
    ReplacementPolicyType policy,
    StorageStats* stats)
    : pool_size_(pool_size_frames),
      disk_(disk_manager),
      stats_(stats),
      pages_(pool_size_frames),
      meta_(pool_size_frames) {
    switch (policy) {
        case ReplacementPolicyType::LRU:
            replacer_ = std::make_unique<LRUReplacer>(pool_size_frames);
            break;
        case ReplacementPolicyType::CLOCK:
            replacer_ = std::make_unique<ClockReplacer>(pool_size_frames);
            break;
        case ReplacementPolicyType::LRU_2:
        default:
            replacer_ = std::make_unique<LRU2Replacer>(pool_size_frames, 2, pool_size_frames * 16);
            break;
    }
    for (size_t i = 0; i < pool_size_frames; ++i) {
        free_list_.push_back(static_cast<frame_id_t>(i));
    }
}

BufferPoolManager::~BufferPoolManager() {
    flush_all_pages();
}

bool BufferPoolManager::find_victim_frame(frame_id_t* out_frame) {
    if (!free_list_.empty()) {
        *out_frame = free_list_.back();
        free_list_.pop_back();
        return true;
    }
    frame_id_t victim_fid = INVALID_FRAME_ID;
    if (!replacer_->victim(&victim_fid)) {
        return false;
    }
    FrameMeta& m = meta_[victim_fid];
    if (m.is_dirty && !allow_steal_) {
        // No-steal policy: never write a dirty page outside a checkpoint.
        // Hand the frame back to the replacer and report exhaustion so the
        // caller can checkpoint (flushing everything at a consistent
        // watermark) and retry.
        replacer_->record_access(victim_fid, m.page_id);
        replacer_->set_evictable(victim_fid, true);
        return false;
    }
    if (m.is_dirty && m.page_id != INVALID_PAGE_ID) {
        disk_->write_page(m.page_id, &pages_[victim_fid]);
        m.is_dirty = false;
    }
    if (m.page_id != INVALID_PAGE_ID) {
        page_table_.erase(m.page_id);
    }
    m.page_id = INVALID_PAGE_ID;
    m.pin_count = 0;
    *out_frame = victim_fid;
    return true;
}

size_t BufferPoolManager::dirty_frame_count() {
    std::lock_guard<std::mutex> lock(latch_);
    size_t n = 0;
    for (const auto& m : meta_) {
        if (m.is_dirty) ++n;
    }
    return n;
}

SlottedPage* BufferPoolManager::fetch_page(page_id_t page_id, bool verify_crc) {
    std::lock_guard<std::mutex> lock(latch_);
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        frame_id_t fid = it->second;
        meta_[fid].pin_count++;
        replacer_->record_access(fid, page_id);
        replacer_->set_evictable(fid, false);
        if (stats_ != nullptr) stats_->buffer_hits++;
        return &pages_[fid];
    }

    if (stats_ != nullptr) stats_->buffer_misses++;
    frame_id_t fid = INVALID_FRAME_ID;
    if (!find_victim_frame(&fid)) {
        return nullptr;
    }

    Status st = disk_->read_page(page_id, &pages_[fid], verify_crc);
    if (!st.ok()) {
        free_list_.push_back(fid);
        return nullptr;
    }

    meta_[fid].page_id = page_id;
    meta_[fid].pin_count = 1;
    meta_[fid].is_dirty = false;
    page_table_[page_id] = fid;
    replacer_->record_access(fid, page_id);
    replacer_->set_evictable(fid, false);
    return &pages_[fid];
}

SlottedPage* BufferPoolManager::new_page(page_id_t* out_page_id, PageType type) {
    std::lock_guard<std::mutex> lock(latch_);
    frame_id_t fid = INVALID_FRAME_ID;
    if (!find_victim_frame(&fid)) {
        return nullptr;
    }

    page_id_t pid = disk_->allocate_page();
    pages_[fid].reset(pid, type);
    pages_[fid].seal_checksum();

    meta_[fid].page_id = pid;
    meta_[fid].pin_count = 1;
    meta_[fid].is_dirty = true;
    page_table_[pid] = fid;
    replacer_->record_access(fid, pid);
    replacer_->set_evictable(fid, false);
    *out_page_id = pid;
    return &pages_[fid];
}

bool BufferPoolManager::unpin_page(page_id_t page_id, bool is_dirty) {
    std::lock_guard<std::mutex> lock(latch_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return false;
    frame_id_t fid = it->second;
    auto& m = meta_[fid];
    if (m.pin_count == 0) return false;
    if (is_dirty) m.is_dirty = true;
    m.pin_count--;
    if (m.pin_count == 0) {
        replacer_->set_evictable(fid, true);
    }
    return true;
}

Status BufferPoolManager::flush_page(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(latch_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return Status::NotFound("Page not in buffer pool");
    frame_id_t fid = it->second;
    Status st = disk_->write_page(page_id, &pages_[fid]);
    if (st.ok()) {
        meta_[fid].is_dirty = false;
    }
    return st;
}

Status BufferPoolManager::flush_all_pages() {
    std::lock_guard<std::mutex> lock(latch_);
    for (auto& [pid, fid] : page_table_) {
        if (meta_[fid].is_dirty) {
            Status st = disk_->write_page(pid, &pages_[fid]);
            if (!st.ok()) return st;
            meta_[fid].is_dirty = false;
        }
    }
    return disk_->sync();
}

void BufferPoolManager::simulate_crash_discard_dirty_frames() {
    std::lock_guard<std::mutex> lock(latch_);
    for (frame_id_t fid = 0; fid < pool_size_; ++fid) {
        replacer_->remove(fid);
        meta_[fid] = FrameMeta{};
    }
    page_table_.clear();
    free_list_.clear();
    for (size_t i = 0; i < pool_size_; ++i) {
        free_list_.push_back(static_cast<frame_id_t>(i));
    }
}

}  // namespace hybriddb
