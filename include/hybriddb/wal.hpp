#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "hybriddb/common.hpp"

namespace hybriddb {

enum class WALOpType : uint8_t {
    PUT = 1,
    DEL = 2,
    CHECKPOINT = 3
};

#pragma pack(push, 1)
struct WALRecordHeader {
    uint32_t crc32;       // IEEE CRC32 of [lsn .. end of value]
    lsn_t lsn;            // Monotonically increasing Log Sequence Number
    uint8_t op_type;      // WALOpType
    uint16_t key_size;    // Key byte length
    uint32_t val_size;    // Value byte length
};
#pragma pack(pop)

static_assert(sizeof(WALRecordHeader) == 19, "WALRecordHeader must be 19 bytes");

struct WALRecord {
    lsn_t lsn{0};
    WALOpType op{WALOpType::PUT};
    std::string key{};
    std::string value{};
};

/**
 * Write-Ahead Log (WAL) Manager with CRC32 verification and crash tail truncation.
 */
class WALManager {
public:
    explicit WALManager(
        const std::filesystem::path& wal_path,
        bool sync_on_write = false,
        StorageStats* stats = nullptr);
    ~WALManager();

    WALManager(const WALManager&) = delete;
    WALManager& operator=(const WALManager&) = delete;

    Status append(WALOpType op, std::string_view key, std::string_view value, lsn_t* out_lsn = nullptr);
    Status sync();
    Status truncate_and_reset();
    void close();

    /**
     * Replay and validate all records in the WAL file.
     * If `recover_torn_tail` is true, any incomplete or CRC-failing final record at the
     * end of the file (caused by a mid-write crash) is safely truncated via ftruncate().
     * Mid-file corruption always returns Status::Corruption.
     */
    Status recover(std::vector<WALRecord>* out_records, bool recover_torn_tail = true, size_t* out_truncated_bytes = nullptr);

    // Fault-injection helpers for crash recovery verification
    Status inject_torn_tail_bytes(size_t num_partial_bytes);
    Status inject_bit_flip_at_offset(size_t byte_offset);

    [[nodiscard]] lsn_t next_lsn() const noexcept { return next_lsn_; }
    void set_next_lsn(lsn_t lsn) noexcept { next_lsn_ = lsn; }
    [[nodiscard]] uint64_t file_size_bytes() const noexcept;

private:
    std::filesystem::path wal_path_;
    int fd_{-1};
    bool sync_on_write_{false};
    lsn_t next_lsn_{1};
    StorageStats* stats_{nullptr};
    mutable std::mutex mutex_;
};

}  // namespace hybriddb
