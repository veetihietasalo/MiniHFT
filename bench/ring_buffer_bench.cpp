#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>

#include "RingBuffer.hpp"

namespace {

// Push + pop on one thread: the queue's own bookkeeping cost with no cross-core traffic.
// Cross-thread latency percentiles need a different harness (roadmap W01).
void BM_RingBuffer_PushPopSameThread(benchmark::State& state) {
    auto rb = std::make_unique<RingBuffer<uint64_t, 1024>>();
    uint64_t i = 0;
    // Google Benchmark's loop variable is unused by design.
    for (auto _ : state) { // NOLINT(clang-analyzer-deadcode.DeadStores)
        uint64_t* slot = rb->claim(); // one in, one out: the queue is never full or empty here
        if (slot == nullptr) {
            state.SkipWithError("claim() failed");
            break;
        }
        *slot = i++;
        rb->publish();
        const uint64_t* front = rb->peek();
        if (front == nullptr) {
            state.SkipWithError("peek() failed");
            break;
        }
        uint64_t value = *front; // DoNotOptimize on a const reference is deprecated: it can still be optimized away
        benchmark::DoNotOptimize(value);
        rb->consume();
    }
}

} // namespace

BENCHMARK(BM_RingBuffer_PushPopSameThread);

BENCHMARK_MAIN();
