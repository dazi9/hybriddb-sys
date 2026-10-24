#include "hybriddb/btree.hpp"

#include <cstring>
#include <stdexcept>

namespace hybriddb {

#pragma pack(push, 1)
struct BTreeMetaPayload {
    uint32_t magic;
    page_id_t root_page_id;
    uint32_t tree_height;
    lsn_t checkpoint_lsn;
};
#pragma pack(pop)

constexpr uint32_t BTREE_META_MAGIC = 0x42545245U;  // "BTRE"

BPlusTreeEngine::BPlusTreeEngine(const BPlusTreeConfig& config)
    : config_(config) {
    std::filesystem::create_directories(config_.db_dir);
    disk_ = std::make_unique<DiskManager>(config_.db_dir / "btree.pages", &stats_);
    bpm_ = std::make_unique<BufferPoolManager>(
        config_.buffer_pool_frames,
        disk_.get(),
        config_.replacer_policy,
        &stats_);
    wal_ = std::make_unique<WALManager>(
        config_.db_dir / "btree.wal",
        config_.sync_wal_on_write,
        &stats_);
    // No-steal buffer pool: dirty pages are never written to disk outside a
    // checkpoint, so crash recovery can replay the WAL onto a consistent
    // checkpoint image (see checkpoint_locked / find_victim_frame).
    bpm_->set_allow_steal(false);
    // Fail closed: corrupt meta page / WAL must refuse the open.
    Status init_st = initialize_or_recover();
    if (!init_st.ok()) {
        throw std::runtime_error("BPlusTreeEngine init failed: " + init_st.message);
    }
}

BPlusTreeEngine::~BPlusTreeEngine() {
    if (!crashed_simulation_) {
        // A failed final checkpoint silently loses every write since the
        // last one; surface it instead of discarding the status.
        Status st = checkpoint();
        if (!st.ok()) {
            std::fprintf(stderr, "[hybriddb] DTOR-CHECKPOINT-FAILED: %s\n",
                         st.message.c_str());
        }
    }
}

page_id_t BPlusTreeEngine::decode_child_pid(std::string_view val) noexcept {
    page_id_t pid = INVALID_PAGE_ID;
    if (val.size() >= sizeof(page_id_t)) {
        std::memcpy(&pid, val.data(), sizeof(page_id_t));
    }
    return pid;
}

std::string BPlusTreeEngine::encode_child_pid(page_id_t pid) {
    std::string s(sizeof(page_id_t), '\0');
    std::memcpy(s.data(), &pid, sizeof(page_id_t));
    return s;
}

Status BPlusTreeEngine::write_meta_page(bool persist_immediately) {
    SlottedPage* meta = bpm_->fetch_page(0, false);
    if (meta == nullptr) return Status::BufferExhausted();
    meta->reset(0, PageType::META_PAGE, checkpoint_lsn_);
    BTreeMetaPayload payload{
        BTREE_META_MAGIC,
        root_page_id_,
        tree_height_,
        checkpoint_lsn_};
    std::string_view val_sv(reinterpret_cast<const char*>(&payload), sizeof(payload));
    meta->upsert_sorted("META", val_sv);
    bpm_->unpin_page(0, true);
    // Persisting the meta page alone must only happen when the tree image it
    // describes is already fully on disk (fresh creation or checkpoint);
    // otherwise a crash would leave the meta pointing at pages that were
    // never written.
    if (persist_immediately) {
        return bpm_->flush_page(0);
    }
    return Status::Ok();
}

Status BPlusTreeEngine::read_meta_page() {
    SlottedPage* meta = bpm_->fetch_page(0, config_.verify_crc);
    if (meta == nullptr) return Status::IOError("Failed to read meta page 0");
    auto [found, idx] = meta->lower_bound_slot("META");
    if (!found || meta->value_at(idx).size() != sizeof(BTreeMetaPayload)) {
        bpm_->unpin_page(0, false);
        return Status::Corruption("Invalid B+ Tree meta page");
    }
    BTreeMetaPayload payload{};
    std::memcpy(&payload, meta->value_at(idx).data(), sizeof(payload));
    bpm_->unpin_page(0, false);
    if (payload.magic != BTREE_META_MAGIC) {
        return Status::Corruption("Meta page magic mismatch");
    }
    root_page_id_ = payload.root_page_id;
    tree_height_ = payload.tree_height;
    checkpoint_lsn_ = payload.checkpoint_lsn;
    return Status::Ok();
}

Status BPlusTreeEngine::initialize_or_recover() {
    if (disk_->num_pages() == 0) {
        page_id_t meta_pid = INVALID_PAGE_ID;
        SlottedPage* meta = bpm_->new_page(&meta_pid, PageType::META_PAGE);
        bpm_->unpin_page(meta_pid, true);
        (void)meta;

        page_id_t root_pid = INVALID_PAGE_ID;
        SlottedPage* root = bpm_->new_page(&root_pid, PageType::BTREE_LEAF);
        bpm_->unpin_page(root_pid, true);
        (void)root;

        root_page_id_ = root_pid;
        tree_height_ = 1;
        checkpoint_lsn_ = 0;
        write_meta_page(true);
    } else {
        Status st = read_meta_page();
        if (!st.ok()) return st;
    }

    // The lsn counter must continue ABOVE the persisted checkpoint watermark.
    // A fresh WALManager would otherwise restart at lsn 1 while meta still
    // carries a large checkpoint_lsn_, and recovery would silently skip every
    // record written by this session.
    wal_->set_next_lsn(checkpoint_lsn_ + 1);

    // Never checkpointed: on-disk tree pages are just the blank creation
    // skeleton (allocate_page writes FREE-typed blank pages). Rebuild from an
    // empty root and replay the whole WAL below.
    if (checkpoint_lsn_ == 0 && disk_->num_pages() > 0) {
        SlottedPage* root = bpm_->fetch_page(root_page_id_, false);
        if (root != nullptr) {
            root->reset(root_page_id_, PageType::BTREE_LEAF, 0);
            root->seal_checksum();
            bpm_->unpin_page(root_page_id_, true);
        }
        tree_height_ = 1;
    }

    // Replay WAL records with lsn > checkpoint_lsn_
    std::vector<WALRecord> records;
    Status wst = wal_->recover(&records, true);
    if (!wst.ok()) return wst;

    for (const auto& rec : records) {
        if (rec.lsn <= checkpoint_lsn_) continue;
        if (rec.op == WALOpType::PUT) {
            apply_put_internal(rec.key, rec.value, rec.lsn);
        } else if (rec.op == WALOpType::DEL) {
            apply_del_internal(rec.key, rec.lsn);
        }
    }
    return Status::Ok();
}

page_id_t BPlusTreeEngine::find_leaf_page(
    std::string_view key,
    std::vector<page_id_t>* out_path) {
    page_id_t curr = root_page_id_;
    while (curr != INVALID_PAGE_ID) {
        SlottedPage* page = bpm_->fetch_page(curr, config_.verify_crc);
        if (page == nullptr) return INVALID_PAGE_ID;

        auto type = static_cast<PageType>(page->header()->page_type);
        if (type == PageType::BTREE_LEAF) {
            bpm_->unpin_page(curr, false);
            return curr;
        }

        if (out_path != nullptr) {
            out_path->push_back(curr);
        }

        // Internal node navigation:
        // prev_page_id holds child P_0 (keys < K_0).
        // Slot i holds key K_i and child P_{i+1} (keys >= K_i).
        uint16_t n = page->header()->num_slots;
        page_id_t next_child = page->header()->prev_page_id;
        auto [found, idx] = page->lower_bound_slot(key);
        if (found) {
            next_child = decode_child_pid(page->value_at(idx));
        } else if (idx > 0 && n > 0) {
            next_child = decode_child_pid(page->value_at(static_cast<uint16_t>(idx - 1)));
        }

        bpm_->unpin_page(curr, false);
        curr = next_child;
    }
    return INVALID_PAGE_ID;
}

Status BPlusTreeEngine::insert_into_parent(
    const std::vector<page_id_t>& path,
    page_id_t left_child,
    std::string_view sep_key,
    page_id_t right_child,
    lsn_t lsn) {
    if (path.empty()) {
        // Splitting the root: allocate a new internal root page
        page_id_t new_root_pid = INVALID_PAGE_ID;
        SlottedPage* new_root = bpm_->new_page(&new_root_pid, PageType::BTREE_INTERNAL);
        if (new_root == nullptr) return Status::BufferExhausted();

        new_root->header()->page_lsn = lsn;
        new_root->header()->prev_page_id = left_child;
        std::string encoded_right = encode_child_pid(right_child);
        new_root->upsert_sorted(sep_key, encoded_right);
        bpm_->unpin_page(new_root_pid, true);

        root_page_id_ = new_root_pid;
        tree_height_++;
        return write_meta_page();
    }

    page_id_t parent_pid = path.back();
    SlottedPage* parent = bpm_->fetch_page(parent_pid, config_.verify_crc);
    if (parent == nullptr) return Status::BufferExhausted();

    std::string encoded_right = encode_child_pid(right_child);
    Status st = parent->upsert_sorted(sep_key, encoded_right);
    if (st.ok()) {
        parent->header()->page_lsn = lsn;
        bpm_->unpin_page(parent_pid, true);
        return Status::Ok();
    }

    // Parent internal node is full -> split internal node
    std::vector<std::pair<std::string, page_id_t>> entries;
    uint16_t n = parent->header()->num_slots;
    entries.reserve(n + 1);
    bool inserted = false;
    for (uint16_t i = 0; i < n; ++i) {
        std::string_view k = parent->key_at(i);
        page_id_t c = decode_child_pid(parent->value_at(i));
        if (!inserted && sep_key < k) {
            entries.emplace_back(std::string(sep_key), right_child);
            inserted = true;
        }
        entries.emplace_back(std::string(k), c);
    }
    if (!inserted) {
        entries.emplace_back(std::string(sep_key), right_child);
    }

    page_id_t orig_leftmost = parent->header()->prev_page_id;
    size_t mid = entries.size() / 2;
    std::string push_up_key = entries[mid].first;
    page_id_t right_leftmost_child = entries[mid].second;

    page_id_t right_internal_pid = INVALID_PAGE_ID;
    SlottedPage* right_internal = bpm_->new_page(&right_internal_pid, PageType::BTREE_INTERNAL);
    if (right_internal == nullptr) {
        bpm_->unpin_page(parent_pid, false);
        return Status::BufferExhausted();
    }

    parent->reset(parent_pid, PageType::BTREE_INTERNAL, lsn);
    parent->header()->prev_page_id = orig_leftmost;
    for (size_t i = 0; i < mid; ++i) {
        std::string enc = encode_child_pid(entries[i].second);
        parent->upsert_sorted(entries[i].first, enc);
    }

    right_internal->reset(right_internal_pid, PageType::BTREE_INTERNAL, lsn);
    right_internal->header()->prev_page_id = right_leftmost_child;
    for (size_t i = mid + 1; i < entries.size(); ++i) {
        std::string enc = encode_child_pid(entries[i].second);
        right_internal->upsert_sorted(entries[i].first, enc);
    }

    bpm_->unpin_page(parent_pid, true);
    bpm_->unpin_page(right_internal_pid, true);

    std::vector<page_id_t> upper_path(path.begin(), path.end() - 1);
    return insert_into_parent(upper_path, parent_pid, push_up_key, right_internal_pid, lsn);
}

Status BPlusTreeEngine::apply_put_internal(
    std::string_view key,
    std::string_view value,
    lsn_t lsn) {
    std::vector<page_id_t> path;
    page_id_t leaf_pid = find_leaf_page(key, &path);
    if (leaf_pid == INVALID_PAGE_ID) return Status::Corruption("Could not locate leaf page");

    SlottedPage* leaf = bpm_->fetch_page(leaf_pid, config_.verify_crc);
    if (leaf == nullptr) return Status::BufferExhausted();

    Status st = leaf->upsert_sorted(key, value, SLOT_FLAG_NORMAL);
    if (st.ok()) {
        leaf->header()->page_lsn = lsn;
        bpm_->unpin_page(leaf_pid, true);
        return Status::Ok();
    }

    // Leaf is full -> Split leaf into two leaves
    std::vector<std::pair<std::string, std::string>> all_records;
    uint16_t n = leaf->header()->num_slots;
    all_records.reserve(n + 1);
    bool placed = false;
    for (uint16_t i = 0; i < n; ++i) {
        std::string_view k = leaf->key_at(i);
        std::string_view v = leaf->value_at(i);
        if (!placed && key <= k) {
            all_records.emplace_back(std::string(key), std::string(value));
            placed = true;
            if (key == k) continue;
        }
        all_records.emplace_back(std::string(k), std::string(v));
    }
    if (!placed) {
        all_records.emplace_back(std::string(key), std::string(value));
    }

    page_id_t right_leaf_pid = INVALID_PAGE_ID;
    SlottedPage* right_leaf = bpm_->new_page(&right_leaf_pid, PageType::BTREE_LEAF);
    if (right_leaf == nullptr) {
        bpm_->unpin_page(leaf_pid, false);
        return Status::BufferExhausted();
    }

    page_id_t old_next = leaf->header()->next_page_id;
    size_t split_idx = all_records.size() / 2;

    leaf->reset(leaf_pid, PageType::BTREE_LEAF, lsn);
    leaf->header()->next_page_id = right_leaf_pid;
    for (size_t i = 0; i < split_idx; ++i) {
        leaf->upsert_sorted(all_records[i].first, all_records[i].second);
    }

    right_leaf->reset(right_leaf_pid, PageType::BTREE_LEAF, lsn);
    right_leaf->header()->next_page_id = old_next;
    for (size_t i = split_idx; i < all_records.size(); ++i) {
        right_leaf->upsert_sorted(all_records[i].first, all_records[i].second);
    }

    std::string separator_key = all_records[split_idx].first;
    bpm_->unpin_page(leaf_pid, true);
    bpm_->unpin_page(right_leaf_pid, true);

    return insert_into_parent(path, leaf_pid, separator_key, right_leaf_pid, lsn);
}

Status BPlusTreeEngine::apply_del_internal(std::string_view key, lsn_t lsn) {
    page_id_t leaf_pid = find_leaf_page(key, nullptr);
    if (leaf_pid == INVALID_PAGE_ID) return Status::NotFound();
    SlottedPage* leaf = bpm_->fetch_page(leaf_pid, config_.verify_crc);
    if (leaf == nullptr) return Status::BufferExhausted();

    bool removed = leaf->erase_slot(key);
    if (removed) {
        leaf->header()->page_lsn = lsn;
    }
    bpm_->unpin_page(leaf_pid, removed);
    return removed ? Status::Ok() : Status::NotFound();
}

Status BPlusTreeEngine::put(std::string_view key, std::string_view value) {
    if (key.empty() || key.size() > 512 || value.size() > 1500) {
        return Status::InvalidArgument("Key or value size out of B+ Tree page bounds");
    }
    std::unique_lock<std::shared_mutex> lock(rw_latch_);
    maybe_checkpoint_for_pressure_locked();
    lsn_t lsn = 0;
    Status wst = wal_->append(WALOpType::PUT, key, value, &lsn);
    if (!wst.ok()) return wst;
    stats_.logical_bytes_written += key.size() + value.size();
    Status st = apply_put_internal(key, value, lsn);
    if (st.code == StatusCode::BUFFER_POOL_EXHAUSTED) {
        // No clean victim frame available (no-steal policy): checkpoint at a
        // consistent watermark, then retry the operation once.
        Status cst = checkpoint_locked();
        if (!cst.ok()) return cst;
        st = apply_put_internal(key, value, lsn);
    }
    return st;
}

Status BPlusTreeEngine::get(std::string_view key, std::string* out_value) {
    if (key.empty()) return Status::InvalidArgument("Empty key");
    std::shared_lock<std::shared_mutex> lock(rw_latch_);
    stats_.logical_reads++;

    page_id_t leaf_pid = find_leaf_page(key, nullptr);
    if (leaf_pid == INVALID_PAGE_ID) return Status::NotFound();

    SlottedPage* leaf = bpm_->fetch_page(leaf_pid, config_.verify_crc);
    if (leaf == nullptr) return Status::BufferExhausted();

    auto [found, idx] = leaf->lower_bound_slot(key);
    if (!found || leaf->is_tombstone(idx)) {
        bpm_->unpin_page(leaf_pid, false);
        return Status::NotFound();
    }
    out_value->assign(leaf->value_at(idx));
    bpm_->unpin_page(leaf_pid, false);
    return Status::Ok();
}

Status BPlusTreeEngine::del(std::string_view key) {
    if (key.empty()) return Status::InvalidArgument("Empty key");
    std::unique_lock<std::shared_mutex> lock(rw_latch_);
    maybe_checkpoint_for_pressure_locked();
    lsn_t lsn = 0;
    Status wst = wal_->append(WALOpType::DEL, key, "", &lsn);
    if (!wst.ok()) return wst;
    stats_.logical_bytes_written += key.size();
    Status st = apply_del_internal(key, lsn);
    if (st.code == StatusCode::BUFFER_POOL_EXHAUSTED) {
        Status cst = checkpoint_locked();
        if (!cst.ok()) return cst;
        st = apply_del_internal(key, lsn);
    }
    return st;
}

Status BPlusTreeEngine::scan(
    std::string_view start_key,
    std::string_view end_key,
    std::vector<std::pair<std::string, std::string>>* out_results,
    size_t limit) {
    std::shared_lock<std::shared_mutex> lock(rw_latch_);
    out_results->clear();
    stats_.logical_reads++;

    page_id_t leaf_pid = find_leaf_page(start_key, nullptr);
    while (leaf_pid != INVALID_PAGE_ID) {
        SlottedPage* leaf = bpm_->fetch_page(leaf_pid, config_.verify_crc);
        if (leaf == nullptr) return Status::BufferExhausted();

        uint16_t n = leaf->header()->num_slots;
        auto [found, start_idx] = leaf->lower_bound_slot(start_key);
        (void)found;
        bool stop = false;
        for (uint16_t i = start_idx; i < n; ++i) {
            std::string_view k = leaf->key_at(i);
            if (!end_key.empty() && k > end_key) {
                stop = true;
                break;
            }
            if (!leaf->is_tombstone(i)) {
                out_results->emplace_back(std::string(k), std::string(leaf->value_at(i)));
                if (limit > 0 && out_results->size() >= limit) {
                    stop = true;
                    break;
                }
            }
        }
        page_id_t next_pid = leaf->header()->next_page_id;
        bpm_->unpin_page(leaf_pid, false);
        if (stop) break;
        leaf_pid = next_pid;
        start_key = "";  // Subsequent leaves start from slot 0
    }
    return Status::Ok();
}

Status BPlusTreeEngine::checkpoint() {
    std::unique_lock<std::shared_mutex> lock(rw_latch_);
    return checkpoint_locked();
}

Status BPlusTreeEngine::checkpoint_locked() {
    Status st = bpm_->flush_all_pages();
    if (!st.ok()) return st;
    checkpoint_lsn_ = wal_->next_lsn() - 1;
    write_meta_page(true);
    return wal_->truncate_and_reset();
}

void BPlusTreeEngine::maybe_checkpoint_for_pressure_locked() {
    // Reserve headroom for one operation (leaf + new sibling + parent chain);
    // if dirty frames approach capacity, checkpoint now so that eviction
    // never has to write a dirty page outside a checkpoint boundary.
    constexpr size_t kDirtyReserve = 6;
    if (bpm_->dirty_frame_count() + kDirtyReserve >= config_.buffer_pool_frames) {
        (void)checkpoint_locked();
    }
}

void BPlusTreeEngine::simulate_crash_without_flush() {
    std::unique_lock<std::shared_mutex> lock(rw_latch_);
    crashed_simulation_ = true;
    bpm_->simulate_crash_discard_dirty_frames();
    wal_->sync();
}

BTreeStructureStats BPlusTreeEngine::inspect_tree() {
    std::shared_lock<std::shared_mutex> lock(rw_latch_);
    BTreeStructureStats s{};
    s.height = tree_height_;
    double fill_sum = 0.0;
    uint32_t total_pages = disk_->num_pages();
    for (page_id_t pid = 1; pid < total_pages; ++pid) {
        SlottedPage* p = bpm_->fetch_page(pid, config_.verify_crc);
        if (p == nullptr) continue;
        auto type = static_cast<PageType>(p->header()->page_type);
        if (type == PageType::BTREE_INTERNAL) {
            s.internal_pages++;
            s.total_slots += p->header()->num_slots;
        } else if (type == PageType::BTREE_LEAF) {
            s.leaf_pages++;
            s.total_slots += p->header()->num_slots;
            double used = static_cast<double>(PAGE_SIZE - p->free_space_bytes());
            fill_sum += used / static_cast<double>(PAGE_SIZE);
        }
        bpm_->unpin_page(pid, false);
    }
    if (s.leaf_pages > 0) {
        s.avg_leaf_fill_factor = fill_sum / static_cast<double>(s.leaf_pages);
    }
    return s;
}

uint64_t BPlusTreeEngine::disk_footprint_bytes() const noexcept {
    return disk_->file_size_bytes() + wal_->file_size_bytes();
}

}  // namespace hybriddb
