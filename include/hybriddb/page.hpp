#pragma once

#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hybriddb/common.hpp"

namespace hybriddb {

enum class PageType : uint16_t {
    FREE = 0,
    BTREE_INTERNAL = 1,
    BTREE_LEAF = 2,
    SST_DATA = 3,
    META_PAGE = 4
};

constexpr uint16_t SLOT_FLAG_NORMAL = 0x0000;
constexpr uint16_t SLOT_FLAG_TOMBSTONE = 0x0001;

#pragma pack(push, 1)
struct PageHeader {
    uint32_t crc32;            // 0..3: IEEE CRC32 of bytes [4..4095]
    lsn_t page_lsn;            // 4..11: Last WAL LSN that modified this page
    page_id_t page_id;         // 12..15: Logical page ID
    uint16_t page_type;        // 16..17: PageType enum
    uint16_t num_slots;        // 18..19: Number of active slots in directory
    uint16_t free_space_end;   // 20..21: Upper boundary of free space (grows downward)
    uint16_t flags;            // 22..23: Reserved flags
    page_id_t next_page_id;    // 24..27: Right sibling / next leaf or right-most child
    page_id_t prev_page_id;    // 28..31: Left sibling / parent or metadata
};

struct SlotEntry {
    uint16_t offset;           // Offset of cell payload within the 4096B page
    uint16_t key_size;         // Byte length of key
    uint16_t val_size;         // Byte length of value (or 4B child page_id for internal nodes)
    uint16_t flags;            // SLOT_FLAG_NORMAL or SLOT_FLAG_TOMBSTONE
};
#pragma pack(pop)

static_assert(sizeof(PageHeader) == 32, "PageHeader must be exactly 32 bytes");
static_assert(sizeof(SlotEntry) == 8, "SlotEntry must be exactly 8 bytes");

/**
 * 4096-byte Slotted Page Layout:
 * +-----------------------------------------------------------------------+
 * | PageHeader (32B) | Slot[0] | Slot[1] ... -->     <-- ... Cell[1] Cell[0] |
 * +-----------------------------------------------------------------------+
 * ^                  ^                               ^                    ^
 * 0                  32                              free_space_end       4096
 */
class alignas(64) SlottedPage {
public:
    SlottedPage() { reset(INVALID_PAGE_ID, PageType::FREE); }

    void reset(page_id_t pid, PageType type, lsn_t lsn = 0) noexcept {
        std::memset(data_, 0, PAGE_SIZE);
        auto* hdr = header();
        hdr->crc32 = 0;
        hdr->page_lsn = lsn;
        hdr->page_id = pid;
        hdr->page_type = static_cast<uint16_t>(type);
        hdr->num_slots = 0;
        hdr->free_space_end = static_cast<uint16_t>(PAGE_SIZE);
        hdr->flags = 0;
        hdr->next_page_id = INVALID_PAGE_ID;
        hdr->prev_page_id = INVALID_PAGE_ID;
    }

    [[nodiscard]] PageHeader* header() noexcept {
        return reinterpret_cast<PageHeader*>(data_);
    }
    [[nodiscard]] const PageHeader* header() const noexcept {
        return reinterpret_cast<const PageHeader*>(data_);
    }

    [[nodiscard]] SlotEntry* slots() noexcept {
        return reinterpret_cast<SlotEntry*>(data_ + sizeof(PageHeader));
    }
    [[nodiscard]] const SlotEntry* slots() const noexcept {
        return reinterpret_cast<const SlotEntry*>(data_ + sizeof(PageHeader));
    }

    [[nodiscard]] uint8_t* raw_data() noexcept { return data_; }
    [[nodiscard]] const uint8_t* raw_data() const noexcept { return data_; }

    [[nodiscard]] size_t free_space_bytes() const noexcept {
        const auto* hdr = header();
        size_t dir_end = sizeof(PageHeader) + static_cast<size_t>(hdr->num_slots) * sizeof(SlotEntry);
        if (hdr->free_space_end < dir_end) return 0;
        return hdr->free_space_end - dir_end;
    }

    [[nodiscard]] std::string_view key_at(uint16_t slot_idx) const noexcept {
        const SlotEntry& s = slots()[slot_idx];
        return {reinterpret_cast<const char*>(data_ + s.offset), s.key_size};
    }

    [[nodiscard]] std::string_view value_at(uint16_t slot_idx) const noexcept {
        const SlotEntry& s = slots()[slot_idx];
        return {reinterpret_cast<const char*>(data_ + s.offset + s.key_size), s.val_size};
    }

    [[nodiscard]] bool is_tombstone(uint16_t slot_idx) const noexcept {
        return (slots()[slot_idx].flags & SLOT_FLAG_TOMBSTONE) != 0;
    }

    // Binary search for key in sorted slot directory: returns (found, index_or_insert_pos)
    [[nodiscard]] std::pair<bool, uint16_t> lower_bound_slot(std::string_view key) const noexcept {
        uint16_t low = 0;
        uint16_t high = header()->num_slots;
        while (low < high) {
            uint16_t mid = static_cast<uint16_t>(low + (high - low) / 2);
            std::string_view mid_key = key_at(mid);
            int cmp = mid_key.compare(key);
            if (cmp == 0) {
                return {true, mid};
            }
            if (cmp < 0) {
                low = static_cast<uint16_t>(mid + 1);
            } else {
                high = mid;
            }
        }
        return {false, low};
    }

    // Insert or update a (key, value) record while keeping slots sorted by key.
    Status upsert_sorted(std::string_view key, std::string_view value, uint16_t flags = SLOT_FLAG_NORMAL) {
        auto [found, idx] = lower_bound_slot(key);
        const size_t payload_len = key.size() + value.size();
        const size_t needed = payload_len + (found ? 0 : sizeof(SlotEntry));

        if (free_space_bytes() < needed) {
            // Try defragmenting dead payload space from previous in-page updates
            compact_payload_if_helpful();
            if (free_space_bytes() < needed) {
                return Status::PageFull();
            }
        }

        auto* hdr = header();
        auto* slot_arr = slots();
        uint16_t new_offset = static_cast<uint16_t>(hdr->free_space_end - payload_len);
        if (payload_len > 0) {
            std::memcpy(data_ + new_offset, key.data(), key.size());
            if (!value.empty()) {
                std::memcpy(data_ + new_offset + key.size(), value.data(), value.size());
            }
        }
        hdr->free_space_end = new_offset;

        if (found) {
            slot_arr[idx].offset = new_offset;
            slot_arr[idx].key_size = static_cast<uint16_t>(key.size());
            slot_arr[idx].val_size = static_cast<uint16_t>(value.size());
            slot_arr[idx].flags = flags;
        } else {
            if (idx < hdr->num_slots) {
                std::memmove(&slot_arr[idx + 1], &slot_arr[idx],
                             static_cast<size_t>(hdr->num_slots - idx) * sizeof(SlotEntry));
            }
            slot_arr[idx].offset = new_offset;
            slot_arr[idx].key_size = static_cast<uint16_t>(key.size());
            slot_arr[idx].val_size = static_cast<uint16_t>(value.size());
            slot_arr[idx].flags = flags;
            hdr->num_slots++;
        }
        return Status::Ok();
    }

    bool erase_slot(std::string_view key) {
        auto [found, idx] = lower_bound_slot(key);
        if (!found) return false;
        auto* hdr = header();
        auto* slot_arr = slots();
        if (idx + 1 < hdr->num_slots) {
            std::memmove(&slot_arr[idx], &slot_arr[idx + 1],
                         static_cast<size_t>(hdr->num_slots - idx - 1) * sizeof(SlotEntry));
        }
        hdr->num_slots--;
        compact_payload_if_helpful();
        return true;
    }

    void compact_payload_if_helpful() {
        auto* hdr = header();
        auto* slot_arr = slots();
        uint8_t temp[PAGE_SIZE];
        uint16_t write_end = static_cast<uint16_t>(PAGE_SIZE);
        for (uint16_t i = 0; i < hdr->num_slots; ++i) {
            uint16_t total = static_cast<uint16_t>(slot_arr[i].key_size + slot_arr[i].val_size);
            write_end = static_cast<uint16_t>(write_end - total);
            if (total > 0) {
                std::memcpy(temp + write_end, data_ + slot_arr[i].offset, total);
            }
            slot_arr[i].offset = write_end;
        }
        size_t copy_len = PAGE_SIZE - write_end;
        if (copy_len > 0) {
            std::memcpy(data_ + write_end, temp + write_end, copy_len);
        }
        hdr->free_space_end = write_end;
    }

    void seal_checksum() noexcept {
        header()->crc32 = crc32_ieee(data_ + sizeof(uint32_t), PAGE_SIZE - sizeof(uint32_t));
    }

    [[nodiscard]] bool verify_checksum() const noexcept {
        uint32_t expected = crc32_ieee(data_ + sizeof(uint32_t), PAGE_SIZE - sizeof(uint32_t));
        return header()->crc32 == expected;
    }

private:
    uint8_t data_[PAGE_SIZE]{};
};

static_assert(sizeof(SlottedPage) == PAGE_SIZE, "SlottedPage must be exactly 4096 bytes");

class DiskManager {
public:
    explicit DiskManager(const std::filesystem::path& db_file, StorageStats* stats = nullptr);
    ~DiskManager();

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    Status read_page(page_id_t page_id, SlottedPage* out_page, bool verify_crc = true);
    Status write_page(page_id_t page_id, SlottedPage* page);
    page_id_t allocate_page();
    Status sync();
    void close();

    [[nodiscard]] uint32_t num_pages() const noexcept { return num_pages_; }
    [[nodiscard]] uint64_t file_size_bytes() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return file_path_; }

private:
    std::filesystem::path file_path_;
    int fd_{-1};
    uint32_t num_pages_{0};
    StorageStats* stats_{nullptr};
    mutable std::mutex io_mutex_;
};

}  // namespace hybriddb
