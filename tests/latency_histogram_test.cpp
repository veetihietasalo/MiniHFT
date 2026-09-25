#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <random>

#include "LatencyHistogram.hpp"

using H = LatencyHistogram;

TEST(LatencyHistogram, ValuesBelow128AreExact) {
    for (uint64_t v = 0; v < 128; ++v) {
        EXPECT_EQ(H::indexOf(v), v);
        EXPECT_EQ(H::lowestEquivalent(H::indexOf(v)), v);
        EXPECT_EQ(H::highestEquivalent(H::indexOf(v)), v);
    }
}

TEST(LatencyHistogram, BucketsTileTheWholeRangeWithoutGaps) {
    for (std::size_t i = 1; i < H::kBucketCount; ++i) {
        ASSERT_EQ(H::lowestEquivalent(i), H::highestEquivalent(i - 1) + 1) << "bucket " << i;
    }
    EXPECT_EQ(H::highestEquivalent(H::kBucketCount - 1), std::numeric_limits<uint64_t>::max());
}

TEST(LatencyHistogram, EveryValueLandsInABucketAtMostOneSixtyFourthWide) {
    std::mt19937_64 rng(7);
    for (int i = 0; i < 200'000; ++i) {
        const unsigned bits = static_cast<unsigned>(rng() % 64) + 1; // spread samples across magnitudes
        const uint64_t v = rng() >> (64 - bits);
        const std::size_t idx = H::indexOf(v);
        ASSERT_LT(idx, H::kBucketCount);

        const uint64_t lo = H::lowestEquivalent(idx);
        const uint64_t hi = H::highestEquivalent(idx);
        ASSERT_LE(lo, v);
        ASSERT_GE(hi, v);
        ASSERT_LE(hi - lo, lo / 64) << "value " << v;
    }
}

TEST(LatencyHistogram, EmptyHistogramReportsZeros) {
    H h;
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.min(), 0u);
    EXPECT_EQ(h.max(), 0u);
    EXPECT_EQ(h.valueAtPercentile(99), 0u);
    EXPECT_EQ(h.mean(), 0.0);
}

TEST(LatencyHistogram, PercentilesOfUniformValuesAreWithinBucketError) {
    auto h = std::make_unique<H>();
    for (uint64_t v = 1; v <= 10'000; ++v) h->record(v);

    EXPECT_EQ(h->count(), 10'000u);
    EXPECT_EQ(h->min(), 1u);
    EXPECT_EQ(h->max(), 10'000u);
    EXPECT_DOUBLE_EQ(h->mean(), 5000.5);
    EXPECT_EQ(h->valueAtPercentile(0), 1u);
    EXPECT_EQ(h->valueAtPercentile(100), 10'000u);

    for (double p : {50.0, 90.0, 99.0, 99.9}) {
        const double exact = p / 100.0 * 10'000;
        const auto got = static_cast<double>(h->valueAtPercentile(p));
        EXPECT_GE(got, exact) << "p" << p; // reported as the top of its bucket: never optimistic
        EXPECT_LE(got, exact * (1.0 + 1.0 / 64)) << "p" << p;
    }
}

TEST(LatencyHistogram, RareOutlierMovesTheMaxButNotTheMedian) {
    auto h = std::make_unique<H>();
    for (int i = 0; i < 9'999; ++i) h->record(100);
    h->record(1'000'000);

    EXPECT_EQ(h->valueAtPercentile(50), 100u);
    EXPECT_EQ(h->valueAtPercentile(99.9), 100u);
    EXPECT_EQ(h->valueAtPercentile(100), 1'000'000u);
    EXPECT_EQ(h->max(), 1'000'000u);
    EXPECT_GT(h->mean(), 199.0); // one outlier doubles the mean: why averages hide tails
}

TEST(LatencyHistogram, ResetClearsEverything) {
    auto h = std::make_unique<H>();
    h->record(5);
    h->record(500);
    h->reset();
    EXPECT_EQ(h->count(), 0u);
    EXPECT_EQ(h->min(), 0u);
    EXPECT_EQ(h->max(), 0u);
    EXPECT_EQ(h->valueAtPercentile(50), 0u);
}
