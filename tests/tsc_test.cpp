#include <gtest/gtest.h>

#include <cstdint>

#include "Tsc.hpp"

TEST(Tsc, CalibrationGivesAPlausibleFrequency) {
    const double ticksPerNs = Tsc::calibrateTicksPerNs(20);
    EXPECT_GT(ticksPerNs, 0.5);  // above 500 MHz
    EXPECT_LT(ticksPerNs, 10.0); // below 10 GHz
}

TEST(Tsc, OrderedReadsNeverGoBackwardsOnOneThread) {
    uint64_t previous = Tsc::readOrdered();
    for (int i = 0; i < 100'000; ++i) {
        const uint64_t now = Tsc::readOrdered();
        ASSERT_GE(now, previous);
        previous = now;
    }
}

TEST(Tsc, TimerOverheadIsSmall) {
    EXPECT_LT(Tsc::measureOverheadTicks(10'000), 1000u);
}
