#include "hybriddb/page.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace hybriddb {

DiskManager::DiskManager(const std::filesystem::path& db_file, StorageStats* stats)
    : file_path_(db_file), stats_(stats) {
    if (file_path_.has_parent_path()) {
        std::filesystem::create_directories(file_path_.parent_path());
    }
    fd_ = ::open(file_path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ >= 0) {
        struct stat st {};
        if (::fstat(fd_, &st) == 0 && st.st_size > 0) {
            num_pages_ = static_cast<uint32_t>(st.st_size / PAGE_SIZE);
        }
    }
}

DiskManager::~DiskManager() {
    close();
}

void DiskManager::close() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (fd_ >= 0) {
        // Only handles that wrote anything need an fsync on close; SST
        // lookups open a fresh handle per search and must not pay for it.
        if (has_written_) {
            ::fsync(fd_);
        }
        ::close(fd_);
        fd_ = -1;
    }
}

Status DiskManager::read_page(page_id_t page_id, SlottedPage* out_page, bool verify_crc) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (fd_ < 0) return Status::IOError("DiskManager is closed");
    if (page_id >= num_pages_) {
        return Status::InvalidArgument("Page ID out of bounds: " + std::to_string(page_id));
    }

    off_t offset = static_cast<off_t>(page_id) * static_cast<off_t>(PAGE_SIZE);
    size_t total_read = 0;
    uint8_t* dst = out_page->raw_data();
    while (total_read < PAGE_SIZE) {
        ssize_t r = ::pread(fd_, dst + total_read, PAGE_SIZE - total_read,
                            offset + static_cast<off_t>(total_read));
        if (r < 0) {
            if (errno == EINTR) continue;
            return Status::IOError(std::string("pread failed: ") + std::strerror(errno));
        }
        if (r == 0) break;
        total_read += static_cast<size_t>(r);
    }

    if (total_read != PAGE_SIZE) {
        return Status::Corruption("Short page read on page " + std::to_string(page_id));
    }
    if (stats_ != nullptr) {
        stats_->physical_page_reads++;
    }
    if (verify_crc && !out_page->verify_checksum()) {
        return Status::Corruption("CRC32 mismatch on page " + std::to_string(page_id));
    }
    return Status::Ok();
}

Status DiskManager::write_page(page_id_t page_id, SlottedPage* page) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (fd_ < 0) return Status::IOError("DiskManager is closed");

    page->header()->page_id = page_id;
    page->seal_checksum();

    off_t offset = static_cast<off_t>(page_id) * static_cast<off_t>(PAGE_SIZE);
    size_t total_written = 0;
    const uint8_t* src = page->raw_data();
    while (total_written < PAGE_SIZE) {
        ssize_t w = ::pwrite(fd_, src + total_written, PAGE_SIZE - total_written,
                             offset + static_cast<off_t>(total_written));
        if (w < 0) {
            if (errno == EINTR) continue;
            return Status::IOError(std::string("pwrite failed: ") + std::strerror(errno));
        }
        total_written += static_cast<size_t>(w);
    }

    if (page_id >= num_pages_) {
        num_pages_ = page_id + 1;
    }
    has_written_ = true;
    if (stats_ != nullptr) {
        stats_->physical_page_writes++;
        stats_->physical_bytes_written += PAGE_SIZE;
    }
    return Status::Ok();
}

page_id_t DiskManager::allocate_page() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    page_id_t pid = num_pages_++;
    SlottedPage blank;
    blank.reset(pid, PageType::FREE);
    blank.seal_checksum();
    off_t offset = static_cast<off_t>(pid) * static_cast<off_t>(PAGE_SIZE);
    ssize_t w = ::pwrite(fd_, blank.raw_data(), PAGE_SIZE, offset);
    (void)w;
    has_written_ = true;
    if (stats_ != nullptr) {
        stats_->physical_page_writes++;
        stats_->physical_bytes_written += PAGE_SIZE;
    }
    return pid;
}

Status DiskManager::sync() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (fd_ < 0) return Status::IOError("DiskManager is closed");
    if (::fsync(fd_) != 0) {
        return Status::IOError(std::string("fsync failed: ") + std::strerror(errno));
    }
    return Status::Ok();
}

uint64_t DiskManager::file_size_bytes() const noexcept {
    std::lock_guard<std::mutex> lock(io_mutex_);
    return static_cast<uint64_t>(num_pages_) * PAGE_SIZE;
}

}  // namespace hybriddb
