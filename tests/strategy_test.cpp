#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <stdexcept>

#include "Order.hpp"
#include "Strategy.hpp"

namespace {

// The parameters of the paper's numerical example: s = 100, T = 1, sigma = 2, gamma = 0.1, k = 1.5.
constexpr double kGamma = 0.1, kSigma = 2.0, kK = 1.5, kT = 1.0, kMid = 100.0;
// (2 / gamma) ln(1 + gamma / k) = 20 ln(16 / 15) = 1.2907704. The paper doesn't print it: its
// Table 1 average spread of 1.49 adds the time-average of gamma sigma^2 (T - t), 0.2.
const double kSpreadAtT = 20.0 * std::log(16.0 / 15.0);

MarketMakerStrategy paperStrategy() { return MarketMakerStrategy(kGamma, kSigma, kK, kT); }

void fill(MarketMakerStrategy& s, Side side, Quantity qty) {
    s.onTrade(Trade{kMid, qty, 1, 2, 0}, side);
}

} // namespace

TEST(Strategy, FlatInventoryQuotesAroundTheMid) {
    const auto s = paperStrategy();
    EXPECT_DOUBLE_EQ(s.getReservationPrice(kMid, 0.0), kMid);
    // gamma sigma^2 T = 0.1 * 4 * 1 = 0.4, plus the time-independent term.
    EXPECT_NEAR(s.getSpread(0.0), 0.4 + kSpreadAtT, 1e-12);
    EXPECT_NEAR(s.getSpread(0.0), 1.6907704, 1e-7);
}

TEST(Strategy, InventorySkewsTheReservationPrice) {
    auto s = paperStrategy();
    fill(s, Side::Buy, 3); // long 3
    // r = 100 - 3 * 0.1 * 4 * (1 - 0.5) = 99.4
    EXPECT_NEAR(s.getReservationPrice(kMid, 0.5), 99.4, 1e-12);
    fill(s, Side::Sell, 5); // short 2
    EXPECT_NEAR(s.getReservationPrice(kMid, 0.5), 100.4, 1e-12);
    EXPECT_EQ(s.getInventory(), -2);
}

TEST(Strategy, SpreadNarrowsTowardTheHorizonAndStaysAfterIt) {
    const auto s = paperStrategy();
    EXPECT_NEAR(s.getSpread(0.5), 0.2 + kSpreadAtT, 1e-12);
    EXPECT_NEAR(s.getSpread(kT), kSpreadAtT, 1e-12);
    EXPECT_NEAR(s.getSpread(2.0 * kT), kSpreadAtT, 1e-12);
}

TEST(Strategy, InventoryStopsMatteringAtTheHorizon) {
    auto s = paperStrategy();
    fill(s, Side::Buy, 7);
    EXPECT_DOUBLE_EQ(s.getReservationPrice(kMid, kT), kMid);
    EXPECT_DOUBLE_EQ(s.getReservationPrice(kMid, 3.0), kMid);
}

TEST(Strategy, SmallRiskAversionGivesTwoOverK) {
    // (2 / gamma) ln(1 + gamma / k) -> 2 / k as gamma -> 0; log1p keeps that accurate.
    const MarketMakerStrategy s(1e-9, kSigma, kK, 0.0);
    EXPECT_NEAR(s.getSpread(0.0), 2.0 / kK, 1e-9);
}

TEST(Strategy, CentRoundingKeepsPricesAlreadyOnTheGrid) {
    // 0.57 * 100 and 1.15 * 100 come out just below 57 and 115 in binary, 0.1 + 0.2 just above 0.3.
    EXPECT_DOUBLE_EQ(MarketMakerStrategy::floorToCent(0.57), 0.57);
    EXPECT_DOUBLE_EQ(MarketMakerStrategy::floorToCent(1.15), 1.15);
    EXPECT_DOUBLE_EQ(MarketMakerStrategy::ceilToCent(0.1 + 0.2), 0.3);
    EXPECT_DOUBLE_EQ(MarketMakerStrategy::floorToCent(99.159), 99.15);
    EXPECT_DOUBLE_EQ(MarketMakerStrategy::ceilToCent(100.851), 100.86);
}

TEST(Strategy, QuotesRoundOutwardToTheCent) {
    auto s = paperStrategy();
    // r = 100, half-spread 0.8453852: 99.1546 and 100.8454 round out to 99.15 and 100.85.
    const auto flat = s.getQuotes(kMid, 0.0);
    EXPECT_DOUBLE_EQ(flat.first, 99.15);
    EXPECT_DOUBLE_EQ(flat.second, 100.85);

    fill(s, Side::Buy, 3);
    // r = 99.4, half-spread 0.7453852: 98.6546 and 100.1454 round out to 98.65 and 100.15.
    const auto skewed = s.getQuotes(kMid, 0.5);
    EXPECT_DOUBLE_EQ(skewed.first, 98.65);
    EXPECT_DOUBLE_EQ(skewed.second, 100.15);

    // Where rounding to the nearest cent would go inward: 99.1596 and 100.8504 must become
    // 99.15 and 100.86, not 99.16 and 100.85.
    const auto halfCent = paperStrategy().getQuotes(100.005, 0.0);
    EXPECT_DOUBLE_EQ(halfCent.first, 99.15);
    EXPECT_DOUBLE_EQ(halfCent.second, 100.86);
}

TEST(Strategy, NanTimeShowsInTheQuotes) {
    auto s = paperStrategy();
    fill(s, Side::Buy, 7);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_TRUE(std::isnan(s.getReservationPrice(kMid, nan)));
    EXPECT_TRUE(std::isnan(s.getSpread(nan)));
}

TEST(Strategy, InventoryHoldsMoreThanAnInt) {
    auto s = paperStrategy();
    fill(s, Side::Buy, 3'000'000'000U);
    fill(s, Side::Buy, 3'000'000'000U);
    EXPECT_EQ(s.getInventory(), 6'000'000'000);
}

TEST(Strategy, RejectsParametersTheModelCannotUse) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(MarketMakerStrategy(0.0, kSigma, kK, kT), std::invalid_argument);
    EXPECT_THROW(MarketMakerStrategy(kGamma, kSigma, 0.0, kT), std::invalid_argument);
    EXPECT_THROW(MarketMakerStrategy(kGamma, -1.0, kK, kT), std::invalid_argument);
    EXPECT_THROW(MarketMakerStrategy(kGamma, kSigma, kK, -1.0), std::invalid_argument);
    EXPECT_THROW(MarketMakerStrategy(nan, kSigma, kK, kT), std::invalid_argument);
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_THROW(MarketMakerStrategy(inf, kSigma, kK, kT), std::invalid_argument);
    EXPECT_THROW(MarketMakerStrategy(kGamma, inf, kK, kT), std::invalid_argument);
    EXPECT_THROW(MarketMakerStrategy(kGamma, kSigma, inf, kT), std::invalid_argument);
    EXPECT_THROW(MarketMakerStrategy(kGamma, kSigma, kK, inf), std::invalid_argument);
    // A subnormal gamma would overflow 2 / gamma.
    EXPECT_THROW(MarketMakerStrategy(std::numeric_limits<double>::denorm_min(), kSigma, kK, kT), std::invalid_argument);
    EXPECT_NO_THROW(MarketMakerStrategy(kGamma, 0.0, kK, 0.0));
}
