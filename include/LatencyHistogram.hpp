#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

// Log-linear histogram in the style of HdrHistogram: fixed memory, O(1) record(),
// no allocation, covers every uint64_t value.
//
// Values below 128 are stored exactly. Above that, each power-of-two range
// [2^n, 2^(n+1)) is split into 64 equal buckets, so a value is reported with at
// most 1/64 (~1.6 %) relative error.
class LatencyHistogram {
public:
    static constexpr unsigned kSubBucketBits = 7;
    static constexpr uint64_t kSubBucketCount = uint64_t{1} << kSubBucketBits; // 128
    static constexpr uint64_t kSubBucketHalf = kSubBucketCount / 2;            // 64
    static constexpr std::size_t kBucketCount = (64 - kSubBucketBits + 2) * kSubBucketHalf;

    // Bucket for a value. `shift` is how many low bits get dropped: 0 below 128,
    // then one more for each doubling.
    static constexpr std::size_t indexOf(uint64_t value) noexcept {
        const unsigned width = static_cast<unsigned>(std::bit_width(value));
        const unsigned shift = width > kSubBucketBits ? width - kSubBucketBits : 0;
        return shift * kSubBucketHalf + static_cast<std::size_t>(value >> shift);
    }

    // Smallest value that maps to bucket `index`.
    static constexpr uint64_t lowestEquivalent(std::size_t index) noexcept {
        if (index < kSubBucketCount) return index;
        const unsigned shift = static_cast<unsigned>(index / kSubBucketHalf) - 1;
        return static_cast<uint64_t>(index - shift * kSubBucketHalf) << shift;
    }

    // Largest value that maps to bucket `index`.
    static constexpr uint64_t highestEquivalent(std::size_t index) noexcept {
        if (index < kSubBucketCount) return index;
        const unsigned shift = static_cast<unsigned>(index / kSubBucketHalf) - 1;
        return lowestEquivalent(index) + ((uint64_t{1} << shift) - 1);
    }

    void record(uint64_t value) noexcept {
        ++counts_[indexOf(value)];
        ++count_;
        sum_ += value;
        min_ = std::min(min_, value);
        max_ = std::max(max_, value);
    }

    uint64_t count() const noexcept { return count_; }
    uint64_t min() const noexcept { return count_ ? min_ : 0; }
    uint64_t max() const noexcept { return max_; }
    double mean() const noexcept { return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0; }

    // Value at or below which `percentile` % of recorded values fall, reported as the
    // top of its bucket (never above the exact max). percentile is in [0, 100].
    uint64_t valueAtPercentile(double percentile) const noexcept {
        if (count_ == 0) return 0;
        if (percentile <= 0.0) return min_;
        const double clamped = std::min(percentile, 100.0);
        const auto rank = std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(clamped / 100.0 * static_cast<double>(count_))));

        uint64_t seen = 0;
        for (std::size_t i = 0; i < kBucketCount; ++i) {
            seen += counts_[i];
            if (seen >= rank) return std::min(highestEquivalent(i), max_);
        }
        return max_;
    }

    void reset() noexcept {
        counts_.fill(0);
        count_ = 0;
        sum_ = 0;
        min_ = std::numeric_limits<uint64_t>::max();
        max_ = 0;
    }

private:
    std::array<uint64_t, kBucketCount> counts_{};
    uint64_t count_ = 0;
    uint64_t sum_ = 0;
    uint64_t min_ = std::numeric_limits<uint64_t>::max();
    uint64_t max_ = 0;
};
