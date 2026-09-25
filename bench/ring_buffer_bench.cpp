#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>

#include "RingBuffer.hpp"

// Push + pop on one thread: the queue's own bookkeeping cost with no cross-core traffic.
// Cross-thread latency percentiles need a different harness (roadmap W01).
static void BM_RingBuffer_PushPopSameThread(benchmark::State& state) {
    auto rb = std::make_unique<RingBuffer<uint64_t, 1024>>();
    uint64_t i = 0;
    for (auto _ : state) {
        *rb->claim() = i++;
        rb->publish();
        uint64_t* slot = rb->peek();
        benchmark::DoNotOptimize(*slot);
        rb->consume();
    }
}
BENCHMARK(BM_RingBuffer_PushPopSameThread);

BENCHMARK_MAIN();
