#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "../bench/Listeners.hpp"
#include "AllocationCounter.hpp"
#include "ItchBook.hpp"
#include "ItchMessages.hpp"
#include "L3OrderBook.hpp"
#include "LatencyHistogram.hpp"
#include "ObjectPool.hpp"
#include "Order.hpp"
#include "OrderBook.hpp"
#include "RingBuffer.hpp"

// Once warmed up, the hot paths must not touch the heap. An allocation can take a lock, can
// make a system call, and makes latency unpredictable.

namespace {

// Where a test stores a pointer to make an allocation observable. C++ lets compilers remove a
// new/delete pair nobody observes (Clang does), which would hide the allocation being counted.
void* volatile g_sink = nullptr;

struct Encoded {
    uint8_t bytes[64];
    size_t length;
    std::span<const uint8_t> message() const { return {bytes, length}; }
};

// One round of order flow for one instrument, ending with an empty book: 400 adds over 20 price
// levels a side, then full executions, partial cancels, replaces at a new price, and deletes.
// Reference numbers start at `refBase`, so every round uses fresh ones, as a real feed does.
std::vector<Encoded> orderFlowRound(uint64_t refBase) {
    constexpr uint16_t kLocate = 1;
    std::vector<Encoded> flow;
    auto emit = [&](auto encode) {
        flow.push_back({});
        flow.back().length = encode(flow.back().bytes);
    };
    auto priceFor = [](char side, uint32_t level) {
        return side == 'B' ? 1'000'000 - level * 100 : 1'000'100 + level * 100;
    };
    auto sideOf = [](uint64_t i) { return i % 2 == 0 ? 'B' : 'S'; };
    auto levelOf = [](uint64_t i) { return static_cast<uint32_t>(i / 2 % 20); };

    for (uint64_t i = 0; i < 400; ++i) {
        emit([&](uint8_t* p) {
            return itch::encodeAddOrder(p, kLocate, i, refBase + i, sideOf(i), 100, "TEST    ", priceFor(sideOf(i), levelOf(i)));
        });
    }
    for (uint64_t i = 0; i < 100; ++i) emit([&](uint8_t* p) { return itch::encodeOrderExecuted(p, kLocate, 0, refBase + i, 100, i); });
    for (uint64_t i = 100; i < 200; ++i) emit([&](uint8_t* p) { return itch::encodeOrderCancel(p, kLocate, 0, refBase + i, 40); });
    for (uint64_t i = 200; i < 300; ++i) {
        const uint32_t newLevel = (levelOf(i) + 1) % 20;
        emit([&](uint8_t* p) {
            return itch::encodeOrderReplace(p, kLocate, 0, refBase + i, refBase + 1000 + i, 100, priceFor(sideOf(i), newLevel));
        });
    }
    for (uint64_t i = 100; i < 200; ++i) emit([&](uint8_t* p) { return itch::encodeOrderDelete(p, kLocate, 0, refBase + i); });
    for (uint64_t i = 300; i < 400; ++i) emit([&](uint8_t* p) { return itch::encodeOrderDelete(p, kLocate, 0, refBase + i); });
    for (uint64_t i = 200; i < 300; ++i) emit([&](uint8_t* p) { return itch::encodeOrderDelete(p, kLocate, 0, refBase + 1000 + i); });
    return flow;
}

// Runs one round to warm the builder up (pool blocks, index table, level vectors all reach
// their working size), then counts the allocations of a second round.
template <typename Builder>
size_t steadyStateAllocations(Builder& builder) {
    const std::vector<Encoded> warmup = orderFlowRound(1);
    const std::vector<Encoded> measured = orderFlowRound(1'000'000);
    for (const Encoded& m : warmup) EXPECT_TRUE(itch::dispatch(m.message(), builder));
    EXPECT_EQ(builder.stats().liveOrders, 0u);
    size_t dispatched = 0;
    const size_t allocations = countAllocations([&] {
        for (const Encoded& m : measured) {
            if (itch::dispatch(m.message(), builder)) ++dispatched;
        }
    });
    EXPECT_EQ(dispatched, measured.size());
    EXPECT_EQ(builder.stats().liveOrders, 0u);
    EXPECT_EQ(builder.stats().unknownRefs, 0u);
    return allocations;
}

} // namespace

TEST(ZeroAlloc, CounterSeesAllocations) { // the counter itself works
    EXPECT_EQ(countAllocations([] {
                  auto p = std::make_unique<int>(1);
                  g_sink = p.get();
              }),
              1u);
    EXPECT_EQ(countAllocations([] {
                  auto p = std::make_unique<RingBuffer<uint64_t, 8>>(); // aligned new
                  g_sink = p.get();
              }),
              1u);
    EXPECT_EQ(countAllocations([] {}), 0u);
}

TEST(ZeroAlloc, RingBufferPushPop) {
    auto ring = std::make_unique<RingBuffer<uint64_t, 1024>>();
    uint64_t sum = 0;
    const size_t allocations = countAllocations([&] {
        for (uint64_t i = 0; i < 100'000; ++i) {
            uint64_t* slot = ring->claim();
            ASSERT_NE(slot, nullptr);
            *slot = i;
            ring->publish();
            const uint64_t* front = ring->peek();
            ASSERT_NE(front, nullptr);
            sum += *front;
            ring->consume();
        }
    });
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(sum, 100'000ull * 99'999 / 2);
}

// release() must never allocate, even past the most objects ever released before: a burst of
// cancels shouldn't hit the heap because the pool's bookkeeping needs to grow.
TEST(ZeroAlloc, ObjectPoolReleaseNeverAllocates) {
    auto pool = std::make_unique<ObjectPool<BookOrder, 4096>>();
    std::vector<BookOrder*> live;
    live.reserve(3000);
    for (int i = 0; i < 3000; ++i) live.push_back(pool->acquire());
    const size_t allocations = countAllocations([&] {
        for (BookOrder* o : live) pool->release(o);
    });
    EXPECT_EQ(allocations, 0u);
}

TEST(ZeroAlloc, LatencyHistogramRecord) {
    auto histogram = std::make_unique<LatencyHistogram>();
    const size_t allocations = countAllocations([&] {
        for (uint64_t v = 0; v < 1'000'000; ++v) histogram->record(v * 7);
    });
    EXPECT_EQ(allocations, 0u);
}

TEST(ZeroAlloc, ItchBookBuilderSteadyState) {
    auto builder = std::make_unique<ItchBookBuilder>(1024);
    EXPECT_EQ(steadyStateAllocations(*builder), 0u);
}

TEST(ZeroAlloc, ItchBookBuilderWithTemplateListenerSteadyState) {
    auto builder = std::make_unique<BasicItchBookBuilder<QuoteCounter>>(1024);
    EXPECT_EQ(steadyStateAllocations(*builder), 0u);
    EXPECT_GT(builder->listener().updates, 0u);
}

TEST(ZeroAlloc, ItchBookBuilderWithVirtualListenerSteadyState) {
    CountingHandler handler;
    auto builder = std::make_unique<BasicItchBookBuilder<VirtualListener>>(1024, VirtualListener{&handler});
    EXPECT_EQ(steadyStateAllocations(*builder), 0u);
    EXPECT_GT(handler.updates, 0u);
}

// Why the original book was replaced: match() returns a std::vector<Trade>, so every trade
// allocates, however warm the book is.
TEST(ZeroAlloc, OriginalOrderBookAllocatesOnEveryMatch) {
    OrderBook book;
    OrderId id = 1;
    for (int i = 0; i < 100; ++i) book.addOrder(Order(id++, Side::Sell, 100.01 + 0.01 * i, 10));
    std::vector<Order> takes;
    takes.reserve(50);
    for (int i = 0; i < 50; ++i) takes.emplace_back(id++, Side::Buy, 1e9, 10); // each fills one resting order
    const size_t allocations = countAllocations([&] {
        for (const Order& o : takes) {
            book.addOrder(o);
            const std::vector<Trade> trades = book.match();
            g_sink = const_cast<Trade*>(trades.data());
            EXPECT_EQ(trades.size(), 1u);
        }
    });
    EXPECT_GE(allocations, takes.size());
}
