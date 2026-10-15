#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hybriddb/common.hpp"

namespace hybriddb {

/**
 * Kirsch-Mitzenmacher (2008) Double-Hashing Bloom Filter:
 *   g_i(x) = (h_1(x) + i * h_2(x)) mod m,  for i = 0 .. k-1
 * where k = clamp(floor(bits_per_key * ln(2)), 1, 30).
 */
class BloomFilter {
public:
    BloomFilter() = default;
    explicit BloomFilter(double bits_per_key);

    void build(const std::vector<std::string>& keys);
    [[nodiscard]] bool may_contain(std::string_view key) const noexcept;

    [[nodiscard]] const std::vector<uint8_t>& raw_bytes() const noexcept { return bits_; }
    void load_from_bytes(const uint8_t* data, size_t len, uint8_t num_probes);

    [[nodiscard]] uint8_t num_probes() const noexcept { return k_; }
    [[nodiscard]] size_t bit_count() const noexcept { return bits_.size() * 8; }
    [[nodiscard]] double bits_per_key() const noexcept { return bits_per_key_; }

    /**
     * Monkey (Dayan, Athanassoulis, Idreos — ACM SIGMOD 2017) Optimal Level-Aware
     * Bloom Filter Bit Allocation.
     *
     * Given entry counts N_0, N_1, ..., N_{L-1} across LSM levels and an average
     * memory budget `avg_bits_per_key` (so total bits M = avg_bits_per_key * sum(N_i)),
     * computes the optimal bits-per-key `b_i` for each level i that minimizes the
     * worst-case lookup false-positive sum R = sum_{i} p_i, subject to sum(N_i * b_i) <= M.
     *
     * By Monkey Section 4.1 (Equations 4-6), optimal FPRs satisfy:
     *   p_i / p_{i+1} = N_i / N_{i+1}
     * which in bits-per-key (since p_i = exp(-b_i * (ln 2)^2)) means:
     *   b_i - b_{i+1} = ln(N_{i+1} / N_i) / (ln 2)^2
     */
    static std::vector<double> compute_monkey_bits_per_key(
        const std::vector<size_t>& level_entry_counts,
        double avg_bits_per_key_budget);

private:
    double bits_per_key_{10.0};
    uint8_t k_{7};
    std::vector<uint8_t> bits_{};
};

}  // namespace hybriddb
