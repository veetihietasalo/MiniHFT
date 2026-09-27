#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "../bench/Listeners.hpp"
#include "ItchBook.hpp"
#include "ItchMessages.hpp"

namespace {

struct Recording {
    std::vector<TopOfBook> updates;
    void onTopOfBook(const TopOfBook& t) { updates.push_back(t); }
};

// Handles every ITCH message type dispatch() passes on except trading actions.
struct MissingTradingAction {
    void onAdd(uint16_t, uint64_t, const itch::AddOrder&) {}
    void onExecuted(uint16_t, uint64_t, const itch::OrderExecuted&) {}
    void onExecutedWithPrice(uint16_t, uint64_t, const itch::OrderExecutedWithPrice&) {}
    void onCancel(uint16_t, uint64_t, const itch::OrderCancel&) {}
    void onDelete(uint16_t, uint64_t, const itch::OrderDelete&) {}
    void onReplace(uint16_t, uint64_t, const itch::OrderReplace&) {}
    void onTrade(uint16_t, uint64_t, const itch::Trade&) {}
    void onStockDirectory(uint16_t, uint64_t, const itch::StockDirectory&) {}
    void onSystemEvent(uint16_t, uint64_t, const itch::SystemEvent&) {}
};

itch::AddOrder add(uint64_t ref, char side, uint32_t shares, uint32_t price) {
    return {ref, side, shares, {'T', 'E', 'S', 'T', ' ', ' ', ' ', ' '}, price};
}

} // namespace

// The concepts are checked at compile time.
static_assert(itch::ItchHandler<ItchBookBuilder>);
static_assert(itch::ItchHandler<BasicItchBookBuilder<QuoteCounter>>);
static_assert(!itch::ItchHandler<MissingTradingAction>);
static_assert(TopOfBookListener<NoListener>);
static_assert(TopOfBookListener<QuoteCounter>);
static_assert(TopOfBookListener<VirtualListener>);
static_assert(!TopOfBookListener<int>);

TEST(TopOfBookListener, HearsOnlyChangesToTheBestBidOrAsk) {
    auto b = std::make_unique<BasicItchBookBuilder<Recording>>(1024);
    auto& seen = b->listener().updates;

    b->onAdd(1, 10, add(1, 'B', 100, 50'0000)); // first bid: new top
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.back().bidPrice, 50'0000u);
    EXPECT_EQ(seen.back().bidShares, 100u);
    EXPECT_EQ(seen.back().askPrice, 0u); // no ask yet
    EXPECT_EQ(seen.back().timestamp, 10u);

    b->onAdd(1, 11, add(2, 'B', 100, 49'0000)); // below the best bid: top unchanged
    EXPECT_EQ(seen.size(), 1u);

    b->onAdd(1, 12, add(3, 'S', 200, 51'0000)); // first ask
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen.back().askPrice, 51'0000u);

    b->onCancel(1, 13, {1, 30}); // top bid shrinks: size change counts
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen.back().bidShares, 70u);

    b->onDelete(1, 14, {2}); // deep order: top unchanged
    EXPECT_EQ(seen.size(), 3u);

    b->onReplace(1, 15, {1, 11, 100, 50'5000}); // top bid moves up: one update, not two
    ASSERT_EQ(seen.size(), 4u);
    EXPECT_EQ(seen.back().bidPrice, 50'5000u);
    EXPECT_EQ(seen.back().bidShares, 100u);
}

TEST(TopOfBookListener, OtherInstrumentsAreReportedWithTheirLocate) {
    auto b = std::make_unique<BasicItchBookBuilder<Recording>>(1024);
    b->onAdd(7, 0, add(1, 'S', 100, 10'0000));
    b->onAdd(9, 0, add(2, 'S', 100, 20'0000));
    ASSERT_EQ(b->listener().updates.size(), 2u);
    EXPECT_EQ(b->listener().updates[0].locate, 7u);
    EXPECT_EQ(b->listener().updates[1].locate, 9u);
}

TEST(TopOfBookListener, TemplateAndVirtualListenersSeeTheSameUpdates) {
    const std::vector<itch::AddOrder> adds = {add(1, 'B', 100, 10'0000), add(2, 'S', 100, 10'0100),
                                              add(3, 'B', 50, 10'0050), add(4, 'B', 10, 9'0000)};
    auto direct = std::make_unique<BasicItchBookBuilder<QuoteCounter>>(1024);
    CountingHandler handler;
    auto indirect = std::make_unique<BasicItchBookBuilder<VirtualListener>>(1024, VirtualListener{&handler});
    for (const auto& a : adds) {
        direct->onAdd(1, 0, a);
        indirect->onAdd(1, 0, a);
    }
    EXPECT_EQ(direct->listener().updates, 3u);
    EXPECT_EQ(direct->listener().updates, handler.updates);
    EXPECT_EQ(direct->listener().checksum, handler.checksum);
}
