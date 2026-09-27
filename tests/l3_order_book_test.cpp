#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "ItchBook.hpp"
#include "ItchMessages.hpp"
#include "L3OrderBook.hpp"

namespace {

constexpr uint16_t kLoc = 7;

itch::AddOrder add(uint64_t ref, char side, uint32_t shares, uint32_t price) {
    return {ref, side, shares, {'T', 'E', 'S', 'T', ' ', ' ', ' ', ' '}, price};
}

// Order refs in one level, front of the queue first.
std::vector<uint64_t> queueAt(const L3OrderBook& book, BookSide side, uint32_t price) {
    std::vector<uint64_t> refs;
    for (const PriceLevel& level : book.levels(side)) {
        if (level.price != price) continue;
        for (const BookOrder* o = level.head; o; o = o->next) refs.push_back(o->ref);
    }
    return refs;
}

class L3Book : public ::testing::Test {
protected:
    // GoogleTest's fixture idiom: each TEST_F body is a subclass that uses `b` directly.
    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    std::unique_ptr<ItchBookBuilder> b = std::make_unique<ItchBookBuilder>(1024);
    const L3OrderBook& book() const { return b->book(kLoc); }
};

} // namespace

TEST_F(L3Book, BestPricesAreAtTheTopOfEachSide) {
    b->onAdd(kLoc, 0, add(1, 'B', 100, 1000'0000));
    b->onAdd(kLoc, 0, add(2, 'B', 100, 1001'0000));
    b->onAdd(kLoc, 0, add(3, 'S', 100, 1003'0000));
    b->onAdd(kLoc, 0, add(4, 'S', 100, 1002'0000));
    ASSERT_NE(book().bestBid(), nullptr);
    ASSERT_NE(book().bestAsk(), nullptr);
    EXPECT_EQ(book().bestBid()->price, 1001'0000u);
    EXPECT_EQ(book().bestAsk()->price, 1002'0000u);
    EXPECT_EQ(book().levels(BookSide::Bid).size(), 2u);
    EXPECT_FALSE(book().crossed());
}

TEST_F(L3Book, SamePriceOrdersQueueInArrivalOrderAndAggregate) {
    b->onAdd(kLoc, 0, add(1, 'B', 100, 50'0000));
    b->onAdd(kLoc, 0, add(2, 'B', 200, 50'0000));
    b->onAdd(kLoc, 0, add(3, 'B', 300, 50'0000));
    EXPECT_EQ(queueAt(book(), BookSide::Bid, 50'0000), (std::vector<uint64_t>{1, 2, 3}));
    EXPECT_EQ(book().bestBid()->shares, 600u);
    EXPECT_EQ(book().bestBid()->orderCount, 3u);
}

TEST_F(L3Book, PartialExecutionKeepsPlaceInLineFullExecutionRemoves) {
    b->onAdd(kLoc, 0, add(1, 'S', 100, 60'0000));
    b->onAdd(kLoc, 0, add(2, 'S', 100, 60'0000));
    b->onExecuted(kLoc, 0, {1, 40, 9});
    EXPECT_EQ(queueAt(book(), BookSide::Ask, 60'0000), (std::vector<uint64_t>{1, 2}));
    EXPECT_EQ(book().bestAsk()->shares, 160u);
    b->onExecuted(kLoc, 0, {1, 60, 10});
    EXPECT_EQ(queueAt(book(), BookSide::Ask, 60'0000), (std::vector<uint64_t>{2}));
    EXPECT_EQ(b->lastPrice(kLoc), 60'0000u);
    EXPECT_EQ(b->stats().liveOrders, 1u);
}

TEST_F(L3Book, CancelDeleteAndEmptyLevelRemoval) {
    b->onAdd(kLoc, 0, add(1, 'B', 100, 70'0000));
    b->onAdd(kLoc, 0, add(2, 'B', 100, 69'0000));
    b->onCancel(kLoc, 0, {1, 30});
    EXPECT_EQ(book().bestBid()->shares, 70u);
    b->onDelete(kLoc, 0, {1});
    EXPECT_EQ(book().bestBid()->price, 69'0000u); // the 70.00 level is gone
    EXPECT_EQ(book().levels(BookSide::Bid).size(), 1u);
    b->onCancel(kLoc, 0, {2, 100}); // cancelling everything removes the order too
    EXPECT_EQ(book().bestBid(), nullptr);
    EXPECT_EQ(b->stats().liveOrders, 0u);
}

TEST_F(L3Book, ReplaceLosesPriorityAndCanMovePrice) {
    b->onAdd(kLoc, 0, add(1, 'B', 100, 80'0000));
    b->onAdd(kLoc, 0, add(2, 'B', 100, 80'0000));
    b->onReplace(kLoc, 0, {1, 11, 150, 80'0000}); // same price: goes to the back of the queue
    EXPECT_EQ(queueAt(book(), BookSide::Bid, 80'0000), (std::vector<uint64_t>{2, 11}));
    EXPECT_EQ(book().bestBid()->shares, 250u);
    b->onReplace(kLoc, 0, {2, 12, 100, 80'0100}); // new price: a new, better level
    EXPECT_EQ(book().bestBid()->price, 80'0100u);
    EXPECT_EQ(queueAt(book(), BookSide::Bid, 80'0000), (std::vector<uint64_t>{11}));
    b->onDelete(kLoc, 0, {1}); // the old ref no longer exists
    EXPECT_EQ(b->stats().unknownRefs, 1u);
}

TEST_F(L3Book, CountsFeedInconsistencies) {
    b->onExecuted(kLoc, 0, {999, 10, 1}); // unknown order
    b->onAdd(kLoc, 0, add(1, 'S', 100, 90'0000));
    b->onExecuted(kLoc, 0, {1, 150, 2}); // more than the order had
    EXPECT_EQ(b->stats().unknownRefs, 1u);
    EXPECT_EQ(b->stats().overExecutions, 1u);
    EXPECT_EQ(book().bestAsk(), nullptr); // the order still left the book

    b->onSystemEvent(0, 0, {'Q'});
    b->onAdd(kLoc, 0, add(2, 'B', 100, 91'0000));
    b->onAdd(kLoc, 0, add(3, 'S', 100, 90'5000)); // below the bid: crossed during market hours
    EXPECT_EQ(b->stats().crossedDuringMarket, 1u);
    EXPECT_EQ(b->stats().crossedWhileTrading, 1u);
}

TEST_F(L3Book, CrossingWhileHaltedIsCountedSeparately) {
    b->onSystemEvent(0, 0, {'Q'});
    b->onTradingAction(kLoc, 0, {{'T', 'E', 'S', 'T', ' ', ' ', ' ', ' '}, 'P', {'L', 'U', 'D', 'P'}}); // LULD pause
    b->onAdd(kLoc, 0, add(1, 'B', 100, 91'0000));
    b->onAdd(kLoc, 0, add(2, 'S', 100, 90'5000)); // orders queue up during the pause: the book crosses
    EXPECT_EQ(b->stats().crossedDuringMarket, 1u);
    EXPECT_EQ(b->stats().crossedWhileTrading, 0u);
    EXPECT_EQ(b->tradingState(kLoc), 'P');
    EXPECT_EQ(b->crossedCount(kLoc), 1u);
}

// A reference number that's already live means the feed or the decoder is wrong. Taking it
// anyway used to overwrite the index entry and strand the first order in the book for good.
TEST_F(L3Book, DuplicateAddLeavesNoUnreachableOrder) {
    b->onAdd(kLoc, 0, add(1, 'B', 100, 50'0000));
    b->onAdd(kLoc, 0, add(1, 'B', 200, 49'0000)); // same reference number again
    EXPECT_EQ(b->stats().duplicateRefs, 1u);
    EXPECT_EQ(book().bestBid()->price, 50'0000u);   // the first order stands
    b->onDelete(kLoc, 0, {1});
    EXPECT_EQ(book().bestBid(), nullptr);
    EXPECT_EQ(b->stats().liveOrders, 0u);
}

TEST_F(L3Book, ReplaceOntoALiveReferenceLeavesNoUnreachableOrder) {
    b->onAdd(kLoc, 0, add(1, 'B', 100, 50'0000));
    b->onAdd(kLoc, 0, add(2, 'B', 100, 49'0000));
    b->onReplace(kLoc, 0, {1, 2, 100, 50'0100}); // new reference 2 is already live
    EXPECT_EQ(b->stats().duplicateRefs, 1u);
    EXPECT_EQ(book().bestBid()->price, 49'0000u);   // order 1 is gone, the original order 2 stands
    b->onDelete(kLoc, 0, {2});
    EXPECT_EQ(book().bestBid(), nullptr);
    EXPECT_EQ(b->stats().liveOrders, 0u);
}

TEST_F(L3Book, InstrumentsAreIndependent) {
    b->onAdd(1, 0, add(1, 'B', 100, 10'0000));
    b->onAdd(2, 0, add(2, 'B', 100, 20'0000));
    b->onStockDirectory(2, 0, {{'M', 'S', 'F', 'T', ' ', ' ', ' ', ' '}});
    EXPECT_EQ(b->book(1).bestBid()->price, 10'0000u);
    EXPECT_EQ(b->book(2).bestBid()->price, 20'0000u);
    EXPECT_EQ(b->symbol(2), "MSFT");
    EXPECT_EQ(b->locateOf("MSFT"), 2);
}
