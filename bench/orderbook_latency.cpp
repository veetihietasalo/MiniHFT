// Latency of OrderBook::addOrder() + match() at a fixed book depth.
//
// The book starts with `depth` resting orders per side. Then each round is two orders:
//   take  an aggressive order that fills exactly the best resting order on one side
//   make  a passive order that puts that side back to `depth`, at a random level
// so the depth stays constant for the whole run and the numbers belong to that depth.
// Orders are built before timing starts; match()'s own allocations and clock reads count.
//
// Usage: orderbook_latency [--depths=10,100,1000] [--events=1000000] [--core=2] [--seed=42]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

#include "BenchCommon.hpp"
#include "LatencyHistogram.hpp"
#include "OrderBook.hpp"
#include "ThreadUtils.hpp"
#include "Tsc.hpp"

namespace {

constexpr Quantity kQty = 10;     // every order has the same size, so one take fills exactly one resting order
constexpr double kTick = 0.01;
constexpr double kBestBid = 99.99;
constexpr double kBestAsk = 100.01;

struct DepthResult {
    LatencyHistogram take;
    LatencyHistogram make;
    uint64_t trades = 0;
};

// Two orders per round: the take, then the make that restores the side it consumed.
std::vector<Order> buildFlow(long long depth, uint64_t rounds, uint64_t seed, OrderId& nextId) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<long long> level(0, depth - 1);
    std::bernoulli_distribution buySide(0.5);

    std::vector<Order> flow;
    flow.reserve(rounds * 2);
    for (uint64_t r = 0; r < rounds; ++r) {
        const long long lvl = level(rng);
        if (buySide(rng)) {
            flow.emplace_back(nextId++, Side::Buy, 1e9, kQty);                          // lifts the best ask
            flow.emplace_back(nextId++, Side::Sell, kBestAsk + kTick * lvl, kQty);       // restores the ask side
        } else {
            flow.emplace_back(nextId++, Side::Sell, kTick, kQty);                        // hits the best bid
            flow.emplace_back(nextId++, Side::Buy, kBestBid - kTick * lvl, kQty);        // restores the bid side
        }
    }
    return flow;
}

std::unique_ptr<DepthResult> run(long long depth, uint64_t events, uint64_t seed) {
    OrderBook book;
    OrderId nextId = 1;
    for (long long i = 0; i < depth; ++i) {
        book.addOrder(Order(nextId++, Side::Buy, kBestBid - kTick * i, kQty));
        book.addOrder(Order(nextId++, Side::Sell, kBestAsk + kTick * i, kQty));
    }

    const uint64_t rounds = events / 2;
    const uint64_t warmupRounds = std::min<uint64_t>(rounds / 10, 50'000);
    const std::vector<Order> flow = buildFlow(depth, warmupRounds + rounds, seed, nextId);

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

    std::printf("MiniHFT orderbook_latency: OrderBook::addOrder() + match() per order\n");
    bench::printMachine(ticksPerNs, overhead);
    std::printf("  thread    core %d (%s)\n", core, pinned ? "pinned" : "NOT pinned");
    std::printf("  load      %llu orders per depth (half take, half make), seed %llu\n",
                static_cast<unsigned long long>(events), static_cast<unsigned long long>(seed));

    bench::printTableHeader("latency (ns)");
    int status = 0;
    for (long long depth : depths) {
        if (depth < 1) continue;
        const auto result = run(depth, events, seed);
        char label[64];
        std::snprintf(label, sizeof label, "depth %lld  take", depth);
        bench::printTableRow(label, result->take, ticksPerNs);
        std::snprintf(label, sizeof label, "depth %lld  make", depth);
        bench::printTableRow(label, result->make, ticksPerNs);

        if (result->trades != result->take.count()) {
            std::printf("WARNING: expected one trade per take, got %llu trades for %llu takes\n",
                        static_cast<unsigned long long>(result->trades),
                        static_cast<unsigned long long>(result->take.count()));
            status = 1;
        }
    }
    return status;
}
