#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hybriddb {

using page_id_t = uint32_t;
using frame_id_t = uint32_t;
using lsn_t = uint64_t;

constexpr page_id_t INVALID_PAGE_ID = 0xFFFFFFFFU;
constexpr frame_id_t INVALID_FRAME_ID = 0xFFFFFFFFU;
constexpr size_t PAGE_SIZE = 4096;

enum class StatusCode : uint8_t {
    OK = 0,
    NOT_FOUND = 1,
    CORRUPTION = 2,
    IO_ERROR = 3,
    INVALID_ARGUMENT = 4,
    PAGE_FULL = 5,
    BUFFER_POOL_EXHAUSTED = 6
};

struct Status {
    StatusCode code{StatusCode::OK};
    std::string message{};

    static Status Ok() { return {StatusCode::OK, ""}; }
    static Status NotFound(std::string msg = "Not found") {
        return {StatusCode::NOT_FOUND, std::move(msg)};
    }
    static Status Corruption(std::string msg) {
        return {StatusCode::CORRUPTION, std::move(msg)};
    }
    static Status IOError(std::string msg) {
        return {StatusCode::IO_ERROR, std::move(msg)};
    }
    static Status InvalidArgument(std::string msg) {
        return {StatusCode::INVALID_ARGUMENT, std::move(msg)};
    }
    static Status PageFull(std::string msg = "Page full") {
        return {StatusCode::PAGE_FULL, std::move(msg)};
    }
    static Status BufferExhausted(std::string msg = "All buffer frames pinned") {
        return {StatusCode::BUFFER_POOL_EXHAUSTED, std::move(msg)};
    }

    [[nodiscard]] bool ok() const noexcept { return code == StatusCode::OK; }
    [[nodiscard]] bool is_not_found() const noexcept { return code == StatusCode::NOT_FOUND; }
    [[nodiscard]] bool is_corruption() const noexcept { return code == StatusCode::CORRUPTION; }
};

struct StorageStats {
    uint64_t logical_bytes_written{0};
    uint64_t physical_bytes_written{0};
    uint64_t logical_reads{0};
    uint64_t physical_page_reads{0};
    uint64_t physical_page_writes{0};
    uint64_t buffer_hits{0};
    uint64_t buffer_misses{0};
    uint64_t bloom_checks{0};
    uint64_t bloom_true_negatives{0};
    uint64_t bloom_false_positives{0};
    uint64_t compactions_run{0};

    [[nodiscard]] double write_amplification() const noexcept {
        if (logical_bytes_written == 0) return 1.0;
        return static_cast<double>(physical_bytes_written) /
               static_cast<double>(logical_bytes_written);
    }

    [[nodiscard]] double read_amplification() const noexcept {
        if (logical_reads == 0) return 0.0;
        return static_cast<double>(physical_page_reads) /
               static_cast<double>(logical_reads);
    }

    [[nodiscard]] double buffer_hit_ratio() const noexcept {
        uint64_t total = buffer_hits + buffer_misses;
        if (total == 0) return 0.0;
        return static_cast<double>(buffer_hits) / static_cast<double>(total);
    }
};

// Fast IEEE 802.3 CRC32 implementation
inline uint32_t crc32_ieee(const void* data, size_t length, uint32_t previous_crc = 0) noexcept {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = ~previous_crc;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = -(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

// Two distinct 32-bit hashes for Kirsch-Mitzenmacher double hashing
inline uint32_t hash_fnv1a_32(std::string_view key) noexcept {
    uint32_t h = 2166136261U;
    for (unsigned char c : key) {
        h ^= c;
        h *= 16777619U;
    }
    return h;
}

inline uint32_t hash_murmur3_32(std::string_view key, uint32_t seed = 0xBC9F1D34U) noexcept {
    const auto* data = reinterpret_cast<const uint8_t*>(key.data());
    const size_t nblocks = key.size() / 4;
    uint32_t h1 = seed;
    constexpr uint32_t c1 = 0xcc9e2d51U;
    constexpr uint32_t c2 = 0x1b873593U;

    for (size_t i = 0; i < nblocks; ++i) {
        uint32_t k1 = static_cast<uint32_t>(data[i * 4 + 0]) |
                      (static_cast<uint32_t>(data[i * 4 + 1]) << 8) |
                      (static_cast<uint32_t>(data[i * 4 + 2]) << 16) |
                      (static_cast<uint32_t>(data[i * 4 + 3]) << 24);
        k1 *= c1;
        k1 = (k1 << 15) | (k1 >> 17);
        k1 *= c2;

        h1 ^= k1;
        h1 = (h1 << 13) | (h1 >> 19);
        h1 = h1 * 5 + 0xe6546b64U;
    }

    const uint8_t* tail = data + nblocks * 4;
    uint32_t k1 = 0;
    switch (key.size() & 3U) {
        case 3:
            k1 ^= static_cast<uint32_t>(tail[2]) << 16;
            [[fallthrough]];
        case 2:
            k1 ^= static_cast<uint32_t>(tail[1]) << 8;
            [[fallthrough]];
        case 1:
            k1 ^= static_cast<uint32_t>(tail[0]);
            k1 *= c1;
            k1 = (k1 << 15) | (k1 >> 17);
            k1 *= c2;
            h1 ^= k1;
    }

    h1 ^= static_cast<uint32_t>(key.size());
    h1 ^= h1 >> 16;
    h1 *= 0x85ebca6bU;
    h1 ^= h1 >> 13;
    h1 *= 0xc2b2ae35U;
    h1 ^= h1 >> 16;
    return h1;
}

}  // namespace hybriddb
