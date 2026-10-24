#include "hybriddb/lsm.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

namespace hybriddb {

// ============================================================================
// 1. SkipListMemTable Implementation
// ============================================================================
SkipListMemTable::SkipListMemTable(uint32_t seed) : rng_(seed) {
    head_ = new Node();
    head_->height = MAX_LEVEL;
    head_->next.fill(nullptr);
}

SkipListMemTable::~SkipListMemTable() {
    clear();
    delete head_;
}

void SkipListMemTable::clear() {
    Node* curr = head_->next[0];
    while (curr != nullptr) {
        Node* next = curr->next[0];
        delete curr;
        curr = next;
    }
    head_->next.fill(nullptr);
    current_max_height_ = 1;
    count_ = 0;
    approx_bytes_ = 0;
}

int SkipListMemTable::random_height() {
    int h = 1;
    while (h < MAX_LEVEL && ((rng_() & 3U) == 0U)) {  // p = 1/4
        ++h;
    }
    return h;
}

void SkipListMemTable::upsert(
    std::string_view key,
    std::string_view value,
    lsn_t lsn,
    bool is_tombstone) {
    std::array<Node*, MAX_LEVEL> update{};
    Node* x = head_;
    for (int i = current_max_height_ - 1; i >= 0; --i) {
        while (x->next[static_cast<size_t>(i)] != nullptr &&
               x->next[static_cast<size_t>(i)]->entry.key < key) {
            x = x->next[static_cast<size_t>(i)];
        }
        update[static_cast<size_t>(i)] = x;
    }

    Node* target = x->next[0];
    if (target != nullptr && target->entry.key == key) {
        approx_bytes_ -= target->entry.value.size();
        target->entry.value.assign(value);
        target->entry.lsn = lsn;
        target->entry.is_tombstone = is_tombstone;
        approx_bytes_ += value.size();
        return;
    }

    int h = random_height();
    if (h > current_max_height_) {
        for (int i = current_max_height_; i < h; ++i) {
            update[static_cast<size_t>(i)] = head_;
        }
        current_max_height_ = h;
    }

    auto* node = new Node();
    node->entry.key.assign(key);
    node->entry.value.assign(value);
    node->entry.lsn = lsn;
    node->entry.is_tombstone = is_tombstone;
    node->height = h;
    node->next.fill(nullptr);

    for (int i = 0; i < h; ++i) {
        node->next[static_cast<size_t>(i)] = update[static_cast<size_t>(i)]->next[static_cast<size_t>(i)];
        update[static_cast<size_t>(i)]->next[static_cast<size_t>(i)] = node;
    }

    count_++;
    approx_bytes_ += sizeof(Node) + key.size() + value.size();
}

bool SkipListMemTable::get(
    std::string_view key,
    std::string* out_value,
    bool* out_tombstone) const {
    const Node* x = head_;
    for (int i = current_max_height_ - 1; i >= 0; --i) {
        while (x->next[static_cast<size_t>(i)] != nullptr &&
               x->next[static_cast<size_t>(i)]->entry.key < key) {
            x = x->next[static_cast<size_t>(i)];
        }
    }
    const Node* target = x->next[0];
    if (target != nullptr && target->entry.key == key) {
        *out_tombstone = target->entry.is_tombstone;
        if (!target->entry.is_tombstone && out_value != nullptr) {
            *out_value = target->entry.value;
        }
        return true;
    }
    return false;
}

std::vector<MemTableEntry> SkipListMemTable::export_sorted() const {
    std::vector<MemTableEntry> out;
    out.reserve(count_);
    const Node* curr = head_->next[0];
    while (curr != nullptr) {
        out.push_back(curr->entry);
        curr = curr->next[0];
    }
    return out;
}

// ============================================================================
// 2. LSMTreeEngine Implementation
// ============================================================================
LSMTreeEngine::LSMTreeEngine(const LSMConfig& config)
    : config_(config),
      memtable_(std::make_unique<SkipListMemTable>()),
      levels_(config.max_levels) {
    std::filesystem::create_directories(config_.db_dir);
    wal_ = std::make_unique<WALManager>(
        config_.db_dir / "lsm.wal",
        config_.sync_wal_on_write,
        &stats_);
    // Fail closed: a corrupt MANIFEST or WAL must refuse the open rather
    // than silently starting with partial level state (stale reads).
    Status init_st = initialize_or_recover();
    if (!init_st.ok()) {
        throw std::runtime_error("LSMTreeEngine init failed: " + init_st.message);
    }
}

LSMTreeEngine::~LSMTreeEngine() {
    if (!crashed_simulation_) {
        if (memtable_->size() > 0) {
            flush_memtable();
        }
        save_manifest();
    }
}

Status LSMTreeEngine::initialize_or_recover() {
    // A corrupt MANIFEST must abort recovery instead of silently shrinking
    // the live SSTable set (which surfaces stale values on later reads).
    Status mst = load_manifest();
    if (!mst.ok()) return mst;

    std::vector<WALRecord> records;
    Status wst = wal_->recover(&records, true);
    if (!wst.ok()) return wst;

    for (const auto& rec : records) {
        if (rec.op == WALOpType::PUT) {
            memtable_->upsert(rec.key, rec.value, rec.lsn, false);
        } else if (rec.op == WALOpType::DEL) {
            memtable_->upsert(rec.key, "", rec.lsn, true);
        }
    }
    return Status::Ok();
}

Status LSMTreeEngine::write_sstable_from_entries(
    uint32_t level,
    const std::vector<MemTableEntry>& entries,
    double bits_per_key,
    SSTableMeta* out_meta) {
    if (entries.empty()) return Status::InvalidArgument("Cannot write empty SSTable");

    uint64_t sst_id = next_sst_id_++;
    std::filesystem::path sst_path =
        config_.db_dir / ("sst_L" + std::to_string(level) + "_" + std::to_string(sst_id) + ".sst");

    DiskManager dm(sst_path, &stats_);
    SlottedPage current_page;
    page_id_t current_pid = 0;
    current_page.reset(0, PageType::SST_DATA);

    std::vector<FencePointer> fences;
    std::vector<std::string> all_keys;
    all_keys.reserve(entries.size());

    std::string page_min_key;
    std::string page_max_key;

    for (const auto& e : entries) {
        all_keys.push_back(e.key);
        uint16_t flags = e.is_tombstone ? SLOT_FLAG_TOMBSTONE : SLOT_FLAG_NORMAL;
        Status st = current_page.upsert_sorted(e.key, e.value, flags);
        if (!st.ok()) {
            // Flush current 4KB block and start a new 4KB block
            current_page.header()->page_lsn = e.lsn;
            dm.write_page(current_pid, &current_page);
            fences.push_back({page_min_key, page_max_key, current_pid});

            current_pid++;
            current_page.reset(current_pid, PageType::SST_DATA);
            page_min_key.clear();
            current_page.upsert_sorted(e.key, e.value, flags);
        }
        if (page_min_key.empty()) page_min_key = e.key;
        page_max_key = e.key;
    }

    if (current_page.header()->num_slots > 0) {
        current_page.header()->page_lsn = entries.back().lsn;
        dm.write_page(current_pid, &current_page);
        fences.push_back({page_min_key, page_max_key, current_pid});
    }
    dm.sync();
    dm.close();

    out_meta->sst_id = sst_id;
    out_meta->level = level;
    out_meta->file_path = sst_path;
    out_meta->num_entries = entries.size();
    out_meta->num_data_pages = static_cast<uint32_t>(fences.size());
    out_meta->min_key = entries.front().key;
    out_meta->max_key = entries.back().key;
    out_meta->fence_pointers = std::move(fences);
    out_meta->cached_keys_for_retune = all_keys;

    if (bits_per_key >= 1.0) {
        out_meta->bloom = BloomFilter(bits_per_key);
        out_meta->bloom.build(all_keys);
    } else {
        out_meta->bloom = BloomFilter(0.0);
    }
    return Status::Ok();
}

Status LSMTreeEngine::read_entries_from_sstable(
    const SSTableMeta& sst,
    std::vector<MemTableEntry>* out_entries) {
    out_entries->clear();
    out_entries->reserve(sst.num_entries);
    DiskManager dm(sst.file_path, &stats_);
    SlottedPage page;
    for (page_id_t pid = 0; pid < sst.num_data_pages; ++pid) {
        Status st = dm.read_page(pid, &page, config_.verify_crc);
        if (!st.ok()) return st;
        uint16_t n = page.header()->num_slots;
        for (uint16_t i = 0; i < n; ++i) {
            MemTableEntry e{};
            e.key.assign(page.key_at(i));
            e.value.assign(page.value_at(i));
            e.lsn = page.header()->page_lsn;
            e.is_tombstone = page.is_tombstone(i);
            out_entries->push_back(std::move(e));
        }
    }
    return Status::Ok();
}

Status LSMTreeEngine::search_in_sstable(
    const SSTableMeta& sst,
    std::string_view key,
    std::string* out_value,
    bool* out_tombstone,
    bool* out_found) {
    *out_found = false;
    if (key < sst.min_key || key > sst.max_key) {
        return Status::Ok();
    }

    // 1. Check Bloom filter if present
    if (sst.bloom.bit_count() > 0) {
        stats_.bloom_checks++;
        if (!sst.bloom.may_contain(key)) {
            stats_.bloom_true_negatives++;
            return Status::Ok();
        }
    }

    // 2. Binary search in-memory Fence Pointers (sparse index by block upper-bound key)
    // to locate the unique candidate 4KB data block within [sst.min_key, sst.max_key].
    auto it = std::lower_bound(
        sst.fence_pointers.begin(),
        sst.fence_pointers.end(),
        key,
        [](const FencePointer& fp, std::string_view target) {
            return fp.max_key < target;
        });

    if (it == sst.fence_pointers.end()) {
        return Status::Ok();
    }

    // 3. Perform exactly 1 physical 4KB page read via DiskManager
    DiskManager dm(sst.file_path, &stats_);
    SlottedPage page;
    Status st = dm.read_page(it->page_id, &page, config_.verify_crc);
    if (!st.ok()) return st;

    auto [found, slot_idx] = page.lower_bound_slot(key);
    if (!found) {
        if (sst.bloom.bit_count() > 0) {
            stats_.bloom_false_positives++;
        }
        return Status::Ok();
    }

    *out_found = true;
    *out_tombstone = page.is_tombstone(slot_idx);
    if (!(*out_tombstone) && out_value != nullptr) {
        out_value->assign(page.value_at(slot_idx));
    }
    return Status::Ok();
}

void LSMTreeEngine::apply_bloom_tuning_internal() {
    std::vector<size_t> level_counts(levels_.size(), 0);
    for (size_t lvl = 0; lvl < levels_.size(); ++lvl) {
        for (const auto& sst : levels_[lvl]) {
            level_counts[lvl] += sst.num_entries;
        }
    }

    std::vector<double> bpk_per_level(levels_.size(), config_.bloom_bits_per_key);
    if (config_.bloom_mode == BloomTuningMode::MONKEY_OPTIMAL) {
        bpk_per_level = BloomFilter::compute_monkey_bits_per_key(
            level_counts, config_.bloom_bits_per_key);
    }

    for (size_t lvl = 0; lvl < levels_.size(); ++lvl) {
        double bpk = bpk_per_level[lvl];
        for (auto& sst : levels_[lvl]) {
            if (bpk >= 1.0 && !sst.cached_keys_for_retune.empty()) {
                sst.bloom = BloomFilter(bpk);
                sst.bloom.build(sst.cached_keys_for_retune);
            } else {
                sst.bloom = BloomFilter(0.0);
            }
        }
    }
}

void LSMTreeEngine::retune_bloom_filters(BloomTuningMode mode, double avg_bits_per_key) {
    std::unique_lock<std::shared_mutex> lock(rw_latch_);
    config_.bloom_mode = mode;
    config_.bloom_bits_per_key = avg_bits_per_key;
    apply_bloom_tuning_internal();
}

Status LSMTreeEngine::flush_memtable() {
    std::unique_lock<std::shared_mutex> lock(rw_latch_);
    if (memtable_->size() == 0) return Status::Ok();

    auto sorted_entries = memtable_->export_sorted();
    SSTableMeta meta{};
    Status st = write_sstable_from_entries(0, sorted_entries, config_.bloom_bits_per_key, &meta);
    if (!st.ok()) return st;

    levels_[0].push_back(std::move(meta));
    memtable_->clear();
    wal_->truncate_and_reset();

    Status cst = maybe_trigger_compaction();
    apply_bloom_tuning_internal();
    save_manifest();
    return cst;
}

Status LSMTreeEngine::maybe_trigger_compaction() {
    if (levels_[0].size() >= config_.l0_compaction_trigger) {
        Status st = compact_level(0);
        if (!st.ok()) return st;
    }
    for (size_t lvl = 1; lvl + 1 < levels_.size(); ++lvl) {
        size_t max_runs = config_.l0_compaction_trigger;
        for (size_t r = 0; r < lvl; ++r) max_runs *= config_.level_size_ratio;
        if (levels_[lvl].size() >= max_runs) {
            Status st = compact_level(static_cast<uint32_t>(lvl));
            if (!st.ok()) return st;
        }
    }
    return Status::Ok();
}

Status LSMTreeEngine::compact_level(uint32_t level) {
    if (level + 1 >= levels_.size() || levels_[level].empty()) {
        return Status::Ok();
    }

    uint32_t next_level = level + 1;
    bool is_bottom_level = (next_level == levels_.size() - 1);

    // Multi-way merge across all runs in `level` and `next_level`
    // In L0, newer SSTables are at the back; so iterating from newest to oldest
    // preserves the latest value for each key!
    std::map<std::string, MemTableEntry> merged;

    auto ingest_sst = [&](const SSTableMeta& sst) -> Status {
        std::vector<MemTableEntry> entries;
        Status st = read_entries_from_sstable(sst, &entries);
        if (!st.ok()) return st;
        for (auto& e : entries) {
            auto it = merged.find(e.key);
            if (it == merged.end()) {
                merged.emplace(e.key, std::move(e));
            }
        }
        return Status::Ok();
    };

    // Newest first from `level`
    for (auto it = levels_[level].rbegin(); it != levels_[level].rend(); ++it) {
        Status st = ingest_sst(*it);
        if (!st.ok()) return st;
    }
    // Then existing runs from `next_level` (newest first)
    for (auto it = levels_[next_level].rbegin(); it != levels_[next_level].rend(); ++it) {
        Status st = ingest_sst(*it);
        if (!st.ok()) return st;
    }

    std::vector<MemTableEntry> compacted_entries;
    compacted_entries.reserve(merged.size());
    for (auto& [k, entry] : merged) {
        if (is_bottom_level && entry.is_tombstone) {
            // Purge tombstones at the deepest level when no older levels exist below
            bool any_deeper_data = false;
            for (size_t d = next_level + 1; d < levels_.size(); ++d) {
                if (!levels_[d].empty()) any_deeper_data = true;
            }
            if (!any_deeper_data) continue;
        }
        compacted_entries.push_back(std::move(entry));
    }

    // Remove old SSTable files on disk
    for (const auto& sst : levels_[level]) {
        std::error_code ec;
        std::filesystem::remove(sst.file_path, ec);
    }
    for (const auto& sst : levels_[next_level]) {
        std::error_code ec;
        std::filesystem::remove(sst.file_path, ec);
    }
    levels_[level].clear();
    levels_[next_level].clear();

    if (!compacted_entries.empty()) {
        SSTableMeta new_sst{};
        Status st = write_sstable_from_entries(
            next_level, compacted_entries, config_.bloom_bits_per_key, &new_sst);
        if (!st.ok()) return st;
        levels_[next_level].push_back(std::move(new_sst));
    }

    stats_.compactions_run++;
    return Status::Ok();
}

Status LSMTreeEngine::compact_all() {
    std::unique_lock<std::shared_mutex> lock(rw_latch_);
    if (memtable_->size() > 0) {
        auto sorted_entries = memtable_->export_sorted();
        SSTableMeta meta{};
        Status st = write_sstable_from_entries(0, sorted_entries, config_.bloom_bits_per_key, &meta);
        if (!st.ok()) return st;
        levels_[0].push_back(std::move(meta));
        memtable_->clear();
        wal_->truncate_and_reset();
    }
    for (uint32_t lvl = 0; lvl + 1 < levels_.size(); ++lvl) {
        if (!levels_[lvl].empty()) {
            Status st = compact_level(lvl);
            if (!st.ok()) return st;
        }
    }
    apply_bloom_tuning_internal();
    return save_manifest();
}

Status LSMTreeEngine::put(std::string_view key, std::string_view value) {
    if (key.empty() || key.size() > 512 || value.size() > 1500) {
        return Status::InvalidArgument("Key or value size out of bounds");
    }
    bool need_flush = false;
    {
        std::unique_lock<std::shared_mutex> lock(rw_latch_);
        lsn_t lsn = 0;
        Status wst = wal_->append(WALOpType::PUT, key, value, &lsn);
        if (!wst.ok()) return wst;
        stats_.logical_bytes_written += key.size() + value.size();
        memtable_->upsert(key, value, lsn, false);
        need_flush = (memtable_->approximate_bytes() >= config_.memtable_max_bytes);
    }
    if (need_flush) {
        return flush_memtable();
    }
    return Status::Ok();
}

Status LSMTreeEngine::del(std::string_view key) {
    if (key.empty()) return Status::InvalidArgument("Empty key");
    bool need_flush = false;
    {
        std::unique_lock<std::shared_mutex> lock(rw_latch_);
        lsn_t lsn = 0;
        Status wst = wal_->append(WALOpType::DEL, key, "", &lsn);
        if (!wst.ok()) return wst;
        stats_.logical_bytes_written += key.size();
        memtable_->upsert(key, "", lsn, true);
        need_flush = (memtable_->approximate_bytes() >= config_.memtable_max_bytes);
    }
    if (need_flush) {
        return flush_memtable();
    }
    return Status::Ok();
}

Status LSMTreeEngine::get(std::string_view key, std::string* out_value) {
    if (key.empty()) return Status::InvalidArgument("Empty key");
    std::shared_lock<std::shared_mutex> lock(rw_latch_);
    stats_.logical_reads++;

    // 1. Check active SkipList MemTable first
    bool is_tomb = false;
    if (memtable_->get(key, out_value, &is_tomb)) {
        return is_tomb ? Status::NotFound() : Status::Ok();
    }

    // 2. Search SSTables from L0 (newest to oldest) down to L_{max-1}
    for (size_t lvl = 0; lvl < levels_.size(); ++lvl) {
        const auto& runs = levels_[lvl];
        for (auto it = runs.rbegin(); it != runs.rend(); ++it) {
            bool found = false;
            Status st = search_in_sstable(*it, key, out_value, &is_tomb, &found);
            if (!st.ok()) return st;
            if (found) {
                return is_tomb ? Status::NotFound() : Status::Ok();
            }
        }
    }
    return Status::NotFound();
}

Status LSMTreeEngine::scan(
    std::string_view start_key,
    std::string_view end_key,
    std::vector<std::pair<std::string, std::string>>* out_results,
    size_t limit) {
    std::shared_lock<std::shared_mutex> lock(rw_latch_);
    out_results->clear();
    stats_.logical_reads++;

    std::map<std::string, std::pair<std::string, bool>> merged;
    for (const auto& e : memtable_->export_sorted()) {
        if (!start_key.empty() && e.key < start_key) continue;
        if (!end_key.empty() && e.key > end_key) continue;
        merged.emplace(e.key, std::make_pair(e.value, e.is_tombstone));
    }

    for (size_t lvl = 0; lvl < levels_.size(); ++lvl) {
        for (auto it = levels_[lvl].rbegin(); it != levels_[lvl].rend(); ++it) {
            if (!start_key.empty() && it->max_key < start_key) continue;
            if (!end_key.empty() && it->min_key > end_key) continue;

            // Use Fence Pointers to read only 4KB data blocks overlapping [start_key, end_key]
            DiskManager dm(it->file_path, &stats_);
            SlottedPage page;
            for (const auto& fp : it->fence_pointers) {
                if (!start_key.empty() && fp.max_key < start_key) continue;
                if (!end_key.empty() && fp.min_key > end_key) break;
                if (!dm.read_page(fp.page_id, &page, config_.verify_crc).ok()) continue;
                uint16_t n = page.header()->num_slots;
                for (uint16_t i = 0; i < n; ++i) {
                    std::string_view k = page.key_at(i);
                    if (!start_key.empty() && k < start_key) continue;
                    if (!end_key.empty() && k > end_key) break;
                    std::string k_str(k);
                    if (merged.find(k_str) == merged.end()) {
                        merged.emplace(
                             std::move(k_str),
                            std::make_pair(std::string(page.value_at(i)), page.is_tombstone(i)));
                    }
                }
            }
        }
    }

    for (const auto& [k, val_tomb] : merged) {
        if (!val_tomb.second) {
            out_results->emplace_back(k, val_tomb.first);
            if (limit > 0 && out_results->size() >= limit) break;
        }
    }
    return Status::Ok();
}

void LSMTreeEngine::simulate_crash_without_flush() {
    std::unique_lock<std::shared_mutex> lock(rw_latch_);
    crashed_simulation_ = true;
    memtable_->clear();
    wal_->sync();
}

Status LSMTreeEngine::save_manifest() {
    std::filesystem::path mpath = config_.db_dir / "MANIFEST";
    std::filesystem::path tmp_path = config_.db_dir / "MANIFEST.tmp";

    std::ostringstream body;
    body << next_sst_id_ << "\n";
    for (size_t lvl = 0; lvl < levels_.size(); ++lvl) {
        for (const auto& sst : levels_[lvl]) {
            body << lvl << " " << sst.sst_id << " " << sst.file_path.filename().string() << "\n";
        }
    }
    const std::string body_str = body.str();
    const uint32_t crc = crc32_ieee(body_str.data(), body_str.size());

    // Atomic manifest publication: write + fsync a temp file, then rename.
    // Without the rename, a crash mid-write leaves a torn MANIFEST that
    // silently drops live SSTables on the next open.
    int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return Status::IOError("Failed to open MANIFEST.tmp for write");
    const std::string payload = "MANIFEST1 " + std::to_string(crc) + "\n" + body_str;
    size_t off = 0;
    while (off < payload.size()) {
        ssize_t w = ::pwrite(fd, payload.data() + off, payload.size() - off,
                             static_cast<off_t>(off));
        if (w <= 0) {
            ::close(fd);
            return Status::IOError("MANIFEST.tmp write failed");
        }
        off += static_cast<size_t>(w);
    }
    ::fsync(fd);
    ::close(fd);

    std::error_code ec;
    std::filesystem::rename(tmp_path, mpath, ec);
    if (ec) return Status::IOError("MANIFEST rename failed: " + ec.message());

    // Persist the rename itself.
    int dir_fd = ::open(config_.db_dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        ::fsync(dir_fd);
        ::close(dir_fd);
    }
    return Status::Ok();
}

Status LSMTreeEngine::load_manifest() {
    std::filesystem::path mpath = config_.db_dir / "MANIFEST";
    if (!std::filesystem::exists(mpath)) return Status::Ok();
    std::ifstream in(mpath, std::ios::binary);
    if (!in.is_open()) return Status::IOError("Failed to open MANIFEST");

    // The manifest is the source of truth for which SSTables are live; a
    // corrupted one must fail the open rather than silently shrink the level
    // state (which surfaces stale values for keys living in dropped SSTs).
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    constexpr const char* kMagic = "MANIFEST1 ";
    constexpr size_t kMagicLen = 10;
    if (content.size() < kMagicLen || content.compare(0, kMagicLen, kMagic) != 0) {
        return Status::Corruption("MANIFEST: missing checksum header");
    }
    const size_t first_nl = content.find('\n', kMagicLen);
    if (first_nl == std::string::npos) {
        return Status::Corruption("MANIFEST: truncated header");
    }
    const uint32_t stored_crc = static_cast<uint32_t>(
        std::stoul(content.substr(kMagicLen, first_nl - kMagicLen)));
    const std::string body = content.substr(first_nl + 1);
    if (crc32_ieee(body.data(), body.size()) != stored_crc) {
        return Status::Corruption("MANIFEST: checksum mismatch");
    }

    std::istringstream body_in(body);
    body_in >> next_sst_id_;
    uint32_t lvl = 0;
    uint64_t sid = 0;
    std::string fname;
    while (body_in >> lvl >> sid >> fname) {
        if (lvl >= levels_.size()) continue;
        std::filesystem::path spath = config_.db_dir / fname;
        if (!std::filesystem::exists(spath)) continue;

        DiskManager dm(spath, nullptr);
        uint32_t npages = dm.num_pages();
        if (npages == 0) continue;

        SSTableMeta meta{};
        meta.sst_id = sid;
        meta.level = lvl;
        meta.file_path = spath;
        meta.num_data_pages = npages;

        SlottedPage page;
        for (page_id_t pid = 0; pid < npages; ++pid) {
            if (!dm.read_page(pid, &page, config_.verify_crc).ok()) break;
            uint16_t n = page.header()->num_slots;
            if (n == 0) continue;
            std::string pmin(page.key_at(0));
            std::string pmax(page.key_at(static_cast<uint16_t>(n - 1)));
            if (meta.min_key.empty()) meta.min_key = pmin;
            meta.max_key = pmax;
            meta.fence_pointers.push_back({pmin, pmax, pid});
            meta.num_entries += n;
            for (uint16_t i = 0; i < n; ++i) {
                meta.cached_keys_for_retune.emplace_back(page.key_at(i));
            }
        }
        levels_[lvl].push_back(std::move(meta));
    }
    apply_bloom_tuning_internal();
    return Status::Ok();
}

std::vector<LSMLevelSummary> LSMTreeEngine::inspect_levels() const {
    std::shared_lock<std::shared_mutex> lock(rw_latch_);
    std::vector<LSMLevelSummary> out;
    for (size_t lvl = 0; lvl < levels_.size(); ++lvl) {
        LSMLevelSummary s{};
        s.level = static_cast<uint32_t>(lvl);
        s.num_sstables = levels_[lvl].size();
        for (const auto& sst : levels_[lvl]) {
            s.total_entries += sst.num_entries;
            s.bits_per_key = sst.bloom.bits_per_key();
            s.filter_bytes += sst.bloom.raw_bytes().size();
        }
        out.push_back(s);
    }
    return out;
}

uint64_t LSMTreeEngine::disk_footprint_bytes() const noexcept {
    uint64_t total = wal_->file_size_bytes();
    for (const auto& runs : levels_) {
        for (const auto& sst : runs) {
            std::error_code ec;
            auto sz = std::filesystem::file_size(sst.file_path, ec);
            if (!ec) total += sz;
        }
    }
    return total;
}

size_t LSMTreeEngine::total_bloom_filter_bytes() const noexcept {
    size_t total = 0;
    for (const auto& runs : levels_) {
        for (const auto& sst : runs) {
            total += sst.bloom.raw_bytes().size();
        }
    }
    return total;
}

}  // namespace hybriddb
