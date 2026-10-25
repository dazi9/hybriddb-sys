#include "hybriddb/bloom.hpp"

#include <algorithm>
#include <cmath>

namespace hybriddb {

BloomFilter::BloomFilter(double bits_per_key)
    : bits_per_key_(std::max(1.0, bits_per_key)) {
    int k = static_cast<int>(bits_per_key_ * 0.69314718056);  // ln(2)
    if (k < 1) k = 1;
    if (k > 30) k = 30;
    k_ = static_cast<uint8_t>(k);
}

void BloomFilter::build(const std::vector<std::string>& keys) {
    size_t n = keys.size();
    size_t total_bits = static_cast<size_t>(std::ceil(static_cast<double>(n) * bits_per_key_));
    if (total_bits < 64) total_bits = 64;
    size_t total_bytes = (total_bits + 7) / 8;
    total_bits = total_bytes * 8;

    bits_.assign(total_bytes, 0);
    for (const auto& key : keys) {
        uint32_t h1 = hash_murmur3_32(key, 0xBC9F1D34U);
        uint32_t h2 = hash_fnv1a_32(key);
        if ((h2 & 1U) == 0) h2 |= 1U;  // Ensure odd step for better cycle coverage

        uint32_t h = h1;
        for (uint8_t j = 0; j < k_; ++j) {
            uint32_t bitpos = h % static_cast<uint32_t>(total_bits);
            bits_[bitpos / 8] |= static_cast<uint8_t>(1U << (bitpos % 8));
            h += h2;
        }
    }
}

bool BloomFilter::may_contain(std::string_view key) const noexcept {
    if (bits_.empty()) return false;
    const size_t total_bits = bits_.size() * 8;

    uint32_t h1 = hash_murmur3_32(key, 0xBC9F1D34U);
    uint32_t h2 = hash_fnv1a_32(key);
    if ((h2 & 1U) == 0) h2 |= 1U;

    uint32_t h = h1;
    for (uint8_t j = 0; j < k_; ++j) {
        uint32_t bitpos = h % static_cast<uint32_t>(total_bits);
        if ((bits_[bitpos / 8] & static_cast<uint8_t>(1U << (bitpos % 8))) == 0) {
            return false;
        }
        h += h2;
    }
    return true;
}

void BloomFilter::load_from_bytes(const uint8_t* data, size_t len, uint8_t num_probes) {
    bits_.assign(data, data + len);
    k_ = (num_probes < 1) ? 1 : (num_probes > 30 ? 30 : num_probes);
}

std::vector<double> BloomFilter::compute_monkey_bits_per_key(
    const std::vector<size_t>& level_entry_counts,
    double avg_bits_per_key_budget) {
    const size_t L = level_entry_counts.size();
    std::vector<double> result(L, avg_bits_per_key_budget);
    size_t total_entries = 0;
    for (size_t n : level_entry_counts) total_entries += n;
    if (L == 0 || total_entries == 0) return result;

    const double total_bits_budget = avg_bits_per_key_budget * static_cast<double>(total_entries);
    const double ln2_sq = std::log(2.0) * std::log(2.0);  // ~0.480453

    // Per Monkey Section 4.1, deeper levels where optimal p_i >= 1 receive 0 bits (L_unfiltered).
    // We iteratively find the active filtered prefix of levels where b_i > 0.
    std::vector<bool> active(L, true);
    for (size_t i = 0; i < L; ++i) {
        if (level_entry_counts[i] == 0) active[i] = false;
    }

    for (size_t iter = 0; iter < L; ++iter) {
        size_t active_entries = 0;
        size_t ref_level = L;
        for (size_t i = 0; i < L; ++i) {
            if (active[i]) {
                active_entries += level_entry_counts[i];
                ref_level = i;
            }
        }
        if (active_entries == 0 || ref_level == L) break;

        // Express each active level's b_i as b_{ref} + delta_i, where:
        //   delta_i = ln(N_{ref} / N_i) / (ln 2)^2
        double weighted_delta_sum = 0.0;
        std::vector<double> deltas(L, 0.0);
        for (size_t i = 0; i < L; ++i) {
            if (!active[i]) continue;
            double ratio = static_cast<double>(level_entry_counts[ref_level]) /
                           static_cast<double>(level_entry_counts[i]);
            deltas[i] = std::log(ratio) / ln2_sq;
            weighted_delta_sum += static_cast<double>(level_entry_counts[i]) * deltas[i];
        }

        double b_ref = (total_bits_budget - weighted_delta_sum) / static_cast<double>(active_entries);
        bool any_non_positive = false;
        for (size_t i = 0; i < L; ++i) {
            if (!active[i]) {
                result[i] = 0.0;
                continue;
            }
            result[i] = b_ref + deltas[i];
            if (result[i] < 1.0) {
                active[i] = false;
                result[i] = 0.0;
                any_non_positive = true;
            }
        }
        if (!any_non_positive) break;
    }

    return result;
}

}  // namespace hybriddb
