#include <gtest/gtest.h>

#include <cstdint>

#include "Tsc.hpp"

#if defined(__aarch64__)

// CNTVCT_EL0 ticks far slower than a TSC (24 MHz on Apple M1, 25 MHz on Ampere Altra,
// 1 GHz from Armv8.6), so there's no useful fixed range. The firmware states the rate in
// CNTFRQ_EL0: the calibration has to agree with it.
TEST(Tsc, CalibrationMatchesTheGenericTimerFrequency) {
    const double nominal = static_cast<double>(Tsc::nominalHz()) / 1e9;
    ASSERT_GT(nominal, 0.001); // above 1 MHz
    ASSERT_LT(nominal, 10.0);  // below 10 GHz
    EXPECT_NEAR(Tsc::calibrateTicksPerNs(20), nominal, nominal * 0.05);
}

#else

TEST(Tsc, CalibrationGivesAPlausibleFrequency) {
    const double ticksPerNs = Tsc::calibrateTicksPerNs(20);
    EXPECT_GT(ticksPerNs, 0.5);  // above 500 MHz
    EXPECT_LT(ticksPerNs, 10.0); // below 10 GHz
}

#endif

TEST(Tsc, OrderedReadsNeverGoBackwardsOnOneThread) {
    uint64_t previous = Tsc::readOrdered();
    for (int i = 0; i < 100'000; ++i) {
        const uint64_t now = Tsc::readOrdered();
        ASSERT_GE(now, previous);
        previous = now;
    }
}

// Below 1000 ticks: a microsecond at 1 GHz, 40 us at 25 MHz.
TEST(Tsc, TimerOverheadIsSmall) {
    EXPECT_LT(Tsc::measureOverheadTicks(10'000), 1000u);
}
