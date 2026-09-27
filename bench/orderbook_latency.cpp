// Latency of one book update at a fixed book depth: the original OrderBook (each side a sorted
// std::vector of orders; matches aggressive orders itself) against the W04 L3 book (price
// levels, a queue per level, O(1) lookup by order reference number).
//
// Each book starts with `depth` resting orders per side, one per price level. Then each round
// is two events:
//   take  the best resting order on one side leaves the book. Old book: an aggressive order is
//         added and matched against it. New book: the feed reports it executed (ITCH 'E').
//   make  a passive order restores that side's depth at a random level.
// Both books see the same sequence of sides and levels, so the depth stays constant and the
// numbers belong to that depth. Old-book orders are built before timing starts; match()'s own
// allocation and clock read count, as they did in W01.
//
// Usage: orderbook_latency [--depths=10,100,1000] [--events=1000000] [--core=2] [--seed=42]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

#include "BenchCommon.hpp"
#include "ItchBook.hpp"
#include "LatencyHistogram.hpp"
#include "OrderBook.hpp"
#include "ThreadUtils.hpp"
#include "Tsc.hpp"

namespace {

constexpr Quantity kQty = 10;     // every order has the same size, so one take removes exactly one resting order
constexpr double kTick = 0.01;
constexpr double kBestBid = 99.99;
constexpr double kBestAsk = 100.01;
constexpr uint32_t kTickItch = 100; // the same prices in ITCH units (4 implied decimals)
constexpr uint32_t kBestBidItch = 999'900;
constexpr uint32_t kBestAskItch = 1'000'100;

struct DepthResult {
    LatencyHistogram take;
    LatencyHistogram make;
    uint64_t trades = 0;
};

struct Round {
    bool buyerTakes; // true: the best ask leaves the book and the ask side is refilled
    long long level; // where the refill lands, in ticks from the best price
};

std::vector<Round> buildRounds(long long depth, uint64_t count, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<long long> level(0, depth - 1);
    std::bernoulli_distribution buySide(0.5);
    std::vector<Round> rounds(count);
    for (Round& r : rounds) {
        r.level = level(rng);
        r.buyerTakes = buySide(rng);
    }
    return rounds;
}

std::unique_ptr<DepthResult> runOld(long long depth, const std::vector<Round>& rounds, uint64_t warmupRounds) {
    OrderBook book;
    OrderId nextId = 1;
    for (long long i = 0; i < depth; ++i) {
        book.addOrder(Order(nextId++, Side::Buy, kBestBid - kTick * i, kQty));
        book.addOrder(Order(nextId++, Side::Sell, kBestAsk + kTick * i, kQty));
    }

    std::vector<Order> flow; // two orders per round: the take, then the make
    flow.reserve(rounds.size() * 2);
    for (const Round& r : rounds) {
        if (r.buyerTakes) {
            flow.emplace_back(nextId++, Side::Buy, 1e9, kQty);                     // lifts the best ask
            flow.emplace_back(nextId++, Side::Sell, kBestAsk + kTick * r.level, kQty);
        } else {
            flow.emplace_back(nextId++, Side::Sell, kTick, kQty);                  // hits the best bid
            flow.emplace_back(nextId++, Side::Buy, kBestBid - kTick * r.level, kQty);
        }
    }

    auto result = std::make_unique<DepthResult>();
    for (std::size_t i = 0; i < flow.size(); ++i) {
        const uint64_t t0 = Tsc::read();
        book.addOrder(flow[i]);
        const std::vector<Trade> trades = book.match();
        const uint64_t t1 = Tsc::readOrdered();

        if (i / 2 < warmupRounds) continue;
        result->trades += trades.size();
        (i % 2 == 0 ? result->take : result->make).record(t1 - t0);
    }
    return result;
}

std::unique_ptr<DepthResult> runNew(long long depth, const std::vector<Round>& rounds, uint64_t warmupRounds) {
    constexpr uint16_t kLocate = 1;
    auto builder = std::make_unique<ItchBookBuilder>(static_cast<size_t>(depth) * 4 + 1024);
    uint64_t nextRef = 1;
    auto add = [&](char side, uint32_t price) { builder->onAdd(kLocate, 0, itch::AddOrder{nextRef++, side, kQty, {}, price}); };

    for (long long i = 0; i < depth; ++i) {
        add('B', kBestBidItch - kTickItch * static_cast<uint32_t>(i));
        add('S', kBestAskItch + kTickItch * static_cast<uint32_t>(i));
    }

    auto result = std::make_unique<DepthResult>();
    for (std::size_t n = 0; n < rounds.size(); ++n) {
        const Round& r = rounds[n];
        const L3OrderBook& book = builder->book(kLocate);
        const PriceLevel* best = r.buyerTakes ? book.bestAsk() : book.bestBid();
        const uint64_t ref = best->head->ref; // the feed names the executed order; look it up outside the timing

        const uint64_t t0 = Tsc::read();
        builder->onExecuted(kLocate, 0, itch::OrderExecuted{ref, kQty, 0});
        const uint64_t t1 = Tsc::readOrdered();
        const uint64_t t2 = Tsc::read();
        if (r.buyerTakes) add('S', kBestAskItch + kTickItch * static_cast<uint32_t>(r.level));
        else add('B', kBestBidItch - kTickItch * static_cast<uint32_t>(r.level));
        const uint64_t t3 = Tsc::readOrdered();

        if (n < warmupRounds) continue;
        ++result->trades;
        result->take.record(t1 - t0);
        result->make.record(t3 - t2);
    }
    if (builder->stats().unknownRefs != 0 || builder->stats().overExecutions != 0) result->trades = 0; // flags a bug below
    return result;
}

} // namespace

int main(int argc, char** argv) {
    if (bench::hasFlag(argc, argv, "--help")) {
        std::printf("usage: orderbook_latency [--depths=10,100,1000] [--events=1000000] [--core=2] [--seed=42]\n");
        return 0;
    }
    const std::vector<long long> depths = bench::argIntList(argc, argv, "depths", {10, 100, 1000});
    const uint64_t events = static_cast<uint64_t>(bench::argInt(argc, argv, "events", 1'000'000));
    const int core = static_cast<int>(bench::argInt(argc, argv, "core", 2));
    const uint64_t seed = static_cast<uint64_t>(bench::argInt(argc, argv, "seed", 42));

    const bool pinned = ThreadUtils::pinThread(core);
    const double ticksPerNs = Tsc::calibrateTicksPerNs();
    const uint64_t overhead = Tsc::measureOverheadTicks();

    std::printf("MiniHFT orderbook_latency: one book update, original OrderBook vs L3 book (W04)\n");
    bench::printMachine(ticksPerNs, overhead);
    std::printf("  thread    core %d (%s)\n", core, pinned ? "pinned" : "NOT pinned");
    std::printf("  load      %llu events per depth (half take, half make), seed %llu\n",
                static_cast<unsigned long long>(events), static_cast<unsigned long long>(seed));

    bench::printTableHeader("latency (ns)");
    int status = 0;
    for (long long depth : depths) {
        if (depth < 1) continue;
        const uint64_t roundCount = events / 2;
        const uint64_t warmupRounds = std::min<uint64_t>(roundCount / 10, 50'000);
        const std::vector<Round> rounds = buildRounds(depth, warmupRounds + roundCount, seed);
        const auto oldBook = runOld(depth, rounds, warmupRounds);
        const auto newBook = runNew(depth, rounds, warmupRounds);

        char label[64];
        std::snprintf(label, sizeof label, "old  depth %lld  take", depth);
        bench::printTableRow(label, oldBook->take, ticksPerNs);
        std::snprintf(label, sizeof label, "new  depth %lld  take", depth);
        bench::printTableRow(label, newBook->take, ticksPerNs);
        std::snprintf(label, sizeof label, "old  depth %lld  make", depth);
        bench::printTableRow(label, oldBook->make, ticksPerNs);
        std::snprintf(label, sizeof label, "new  depth %lld  make", depth);
        bench::printTableRow(label, newBook->make, ticksPerNs);

        for (const auto* r : {oldBook.get(), newBook.get()}) {
            if (r->trades != r->take.count()) {
                std::printf("WARNING: expected one trade per take, got %llu trades for %llu takes\n",
                            static_cast<unsigned long long>(r->trades), static_cast<unsigned long long>(r->take.count()));
                status = 1;
            }
        }
    }
    return status;
}
