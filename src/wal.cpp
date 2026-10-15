#include "hybriddb/wal.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace hybriddb {

WALManager::WALManager(
    const std::filesystem::path& wal_path,
    bool sync_on_write,
    StorageStats* stats)
    : wal_path_(wal_path), sync_on_write_(sync_on_write), stats_(stats) {
    if (wal_path_.has_parent_path()) {
        std::filesystem::create_directories(wal_path_.parent_path());
    }
    fd_ = ::open(wal_path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ >= 0) {
        ::lseek(fd_, 0, SEEK_END);
    }
}

WALManager::~WALManager() {
    close();
}

void WALManager::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0) {
        ::fsync(fd_);
        ::close(fd_);
        fd_ = -1;
    }
}

Status WALManager::append(
    WALOpType op,
    std::string_view key,
    std::string_view value,
    lsn_t* out_lsn) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) return Status::IOError("WAL file is closed");

    lsn_t lsn = next_lsn_++;
    if (out_lsn != nullptr) *out_lsn = lsn;

    WALRecordHeader hdr{};
    hdr.lsn = lsn;
    hdr.op_type = static_cast<uint8_t>(op);
    hdr.key_size = static_cast<uint16_t>(key.size());
    hdr.val_size = static_cast<uint32_t>(value.size());

    const size_t total_len = sizeof(WALRecordHeader) + key.size() + value.size();
    std::vector<uint8_t> buf(total_len);
    std::memcpy(buf.data(), &hdr, sizeof(WALRecordHeader));
    if (!key.empty()) {
        std::memcpy(buf.data() + sizeof(WALRecordHeader), key.data(), key.size());
    }
    if (!value.empty()) {
        std::memcpy(buf.data() + sizeof(WALRecordHeader) + key.size(), value.data(), value.size());
    }

    uint32_t crc = crc32_ieee(buf.data() + sizeof(uint32_t), total_len - sizeof(uint32_t));
    std::memcpy(buf.data(), &crc, sizeof(uint32_t));

    off_t end_pos = ::lseek(fd_, 0, SEEK_END);
    (void)end_pos;
    size_t written = 0;
    while (written < total_len) {
        ssize_t w = ::write(fd_, buf.data() + written, total_len - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            return Status::IOError(std::string("WAL write failed: ") + std::strerror(errno));
        }
        written += static_cast<size_t>(w);
    }

    if (stats_ != nullptr) {
        stats_->physical_bytes_written += total_len;
    }
    if (sync_on_write_) {
        ::fsync(fd_);
    }
    return Status::Ok();
}

Status WALManager::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) return Status::IOError("WAL file is closed");
    if (::fsync(fd_) != 0) {
        return Status::IOError(std::string("WAL fsync failed: ") + std::strerror(errno));
    }
    return Status::Ok();
}

Status WALManager::truncate_and_reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) return Status::IOError("WAL file is closed");
    if (::ftruncate(fd_, 0) != 0) {
        return Status::IOError("WAL ftruncate failed");
    }
    ::lseek(fd_, 0, SEEK_SET);
    return Status::Ok();
}

Status WALManager::recover(
    std::vector<WALRecord>* out_records,
    bool recover_torn_tail,
    size_t* out_truncated_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) return Status::IOError("WAL file is closed");
    out_records->clear();
    if (out_truncated_bytes != nullptr) *out_truncated_bytes = 0;

    struct stat st {};
    if (::fstat(fd_, &st) != 0) {
        return Status::IOError("WAL fstat failed");
    }
    const size_t file_size = static_cast<size_t>(st.st_size);
    if (file_size == 0) return Status::Ok();

    std::vector<uint8_t> raw(file_size);
    size_t total_read = 0;
    while (total_read < file_size) {
        ssize_t r = ::pread(fd_, raw.data() + total_read, file_size - total_read,
                            static_cast<off_t>(total_read));
        if (r <= 0) break;
        total_read += static_cast<size_t>(r);
    }

    size_t offset = 0;
    lsn_t max_lsn = 0;
    while (offset < file_size) {
        if (offset + sizeof(WALRecordHeader) > file_size) {
            if (recover_torn_tail) {
                size_t dead = file_size - offset;
                if (out_truncated_bytes != nullptr) *out_truncated_bytes = dead;
                int rc = ::ftruncate(fd_, static_cast<off_t>(offset));
                (void)rc;
                ::lseek(fd_, static_cast<off_t>(offset), SEEK_SET);
                break;
            }
            return Status::Corruption("Truncated WAL header at offset " + std::to_string(offset));
        }

        WALRecordHeader hdr{};
        std::memcpy(&hdr, raw.data() + offset, sizeof(WALRecordHeader));
        size_t rec_len = sizeof(WALRecordHeader) + hdr.key_size + hdr.val_size;

        if (offset + rec_len > file_size) {
            if (recover_torn_tail) {
                size_t dead = file_size - offset;
                if (out_truncated_bytes != nullptr) *out_truncated_bytes = dead;
                int rc = ::ftruncate(fd_, static_cast<off_t>(offset));
                (void)rc;
                ::lseek(fd_, static_cast<off_t>(offset), SEEK_SET);
                break;
            }
            return Status::Corruption("Truncated WAL payload at offset " + std::to_string(offset));
        }

        uint32_t computed_crc = crc32_ieee(
            raw.data() + offset + sizeof(uint32_t),
            rec_len - sizeof(uint32_t));
        if (computed_crc != hdr.crc32) {
            bool is_tail_record = (offset + rec_len == file_size);
            if (recover_torn_tail && is_tail_record) {
                size_t dead = file_size - offset;
                if (out_truncated_bytes != nullptr) *out_truncated_bytes = dead;
                int rc = ::ftruncate(fd_, static_cast<off_t>(offset));
                (void)rc;
                ::lseek(fd_, static_cast<off_t>(offset), SEEK_SET);
                break;
            }
            return Status::Corruption("WAL CRC32 mismatch at offset " + std::to_string(offset));
        }

        WALRecord rec{};
        rec.lsn = hdr.lsn;
        rec.op = static_cast<WALOpType>(hdr.op_type);
        const char* kptr = reinterpret_cast<const char*>(raw.data() + offset + sizeof(WALRecordHeader));
        const char* vptr = kptr + hdr.key_size;
        rec.key.assign(kptr, hdr.key_size);
        rec.value.assign(vptr, hdr.val_size);
        out_records->push_back(std::move(rec));

        if (hdr.lsn > max_lsn) max_lsn = hdr.lsn;
        offset += rec_len;
    }

    if (max_lsn >= next_lsn_) {
        next_lsn_ = max_lsn + 1;
    }
    return Status::Ok();
}

Status WALManager::inject_torn_tail_bytes(size_t num_partial_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) return Status::IOError("WAL closed");
    std::vector<uint8_t> garbage(num_partial_bytes, 0xAB);
    ::lseek(fd_, 0, SEEK_END);
    ssize_t w = ::write(fd_, garbage.data(), garbage.size());
    (void)w;
    ::fsync(fd_);
    return Status::Ok();
}

Status WALManager::inject_bit_flip_at_offset(size_t byte_offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) return Status::IOError("WAL closed");
    uint8_t b = 0;
    if (::pread(fd_, &b, 1, static_cast<off_t>(byte_offset)) != 1) {
        return Status::IOError("Failed to read byte at offset");
    }
    b ^= 0xFF;
    if (::pwrite(fd_, &b, 1, static_cast<off_t>(byte_offset)) != 1) {
        return Status::IOError("Failed to write flipped byte");
    }
    ::fsync(fd_);
    return Status::Ok();
}

uint64_t WALManager::file_size_bytes() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) return 0;
    struct stat st {};
    if (::fstat(fd_, &st) != 0) return 0;
    return static_cast<uint64_t>(st.st_size);
}

}  // namespace hybriddb
