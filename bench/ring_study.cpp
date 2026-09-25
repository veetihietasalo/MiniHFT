// RingBuffer design changes measured one at a time (roadmap W03).
//
// Every variant runs on the same cores, in the same process, with the same settings:
//   burst   messages sent back-to-back; ns per message is the median of --reps runs
//           (throughput: how fast the pair can move data)
//   paced   one message every --interval-ns; one-way send->receive percentiles
//           (latency: how long one message takes when the queue is nearly empty)
//
// Usage: ring_study [--producer=2] [--consumer=3] [--burst=10000000] [--reps=5]
//                   [--paced=500000] [--interval-ns=1000] [--high-priority]
//                   [--producer-work-ns=0] [--consumer-work-ns=0]
// The work options add busy work per message on one side, so the queue settles near empty
// (slow producer) or near full (slow consumer) instead of swinging between the two.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "BenchCommon.hpp"
#include "RingBuffer.hpp"
#include "RingHarness.hpp"
#include "RingVariants.hpp"
#include "Tsc.hpp"

namespace {

constexpr size_t kSize = 1024;

struct StudyConfig {
    RingRunConfig base;
    uint64_t burstMessages = 10'000'000;
    int reps = 5;
    uint64_t pacedMessages = 500'000;
    uint64_t intervalTicks = 0;
    double ticksPerNs = 1.0;
};

bool g_failed = false;

template <typename Queue>
void study(const char* name, const StudyConfig& s) {
    std::vector<double> nsPerMessage;
    for (int rep = 0; rep < s.reps; ++rep) {
        RingRunConfig burst = s.base;
        burst.messages = s.burstMessages;
        burst.warmup = s.burstMessages / 10;
        burst.intervalTicks = 0;
        burst.recordLatency = false;
        const auto r = runRing<Queue>(burst, s.ticksPerNs);
        if (r->outOfOrder != 0) g_failed = true;
        nsPerMessage.push_back(r->nsPerMessage(burst.messages, s.ticksPerNs));
    }
    std::sort(nsPerMessage.begin(), nsPerMessage.end());
    const double median = nsPerMessage[nsPerMessage.size() / 2];

    RingRunConfig paced = s.base;
    paced.messages = s.pacedMessages;
    paced.warmup = s.pacedMessages / 10;
    paced.intervalTicks = s.intervalTicks;
    paced.recordLatency = true;
    const auto p = runRing<Queue>(paced, s.ticksPerNs);
    if (p->outOfOrder != 0 || p->negativeDeltas != 0) g_failed = true;

    auto ns = [&](uint64_t ticks) { return static_cast<double>(ticks) / s.ticksPerNs; };
    std::printf("%-34s %10.2f %10.1f %10.1f %10.1f %10.1f\n", name, median, 1e3 / median,
                ns(p->sendToReceive.valueAtPercentile(50)), ns(p->sendToReceive.valueAtPercentile(90)),
                ns(p->sendToReceive.valueAtPercentile(99)));
}

} // namespace

int main(int argc, char** argv) {
    if (bench::hasFlag(argc, argv, "--help")) {
        std::printf("usage: ring_study [--producer=2] [--consumer=3] [--burst=10000000] [--reps=5] [--paced=500000] [--interval-ns=1000] [--high-priority] [--producer-work-ns=0] [--consumer-work-ns=0]\n");
        return 0;
    }
    const long long producerWorkNs = bench::argInt(argc, argv, "producer-work-ns", 0);
    const long long consumerWorkNs = bench::argInt(argc, argv, "consumer-work-ns", 0);
    StudyConfig s;
    s.ticksPerNs = Tsc::calibrateTicksPerNs();
    const uint64_t overhead = Tsc::measureOverheadTicks();
    const uint64_t intervalNs = static_cast<uint64_t>(bench::argInt(argc, argv, "interval-ns", 1000));
    s.base.producerCore = static_cast<int>(bench::argInt(argc, argv, "producer", 2));
    s.base.consumerCore = static_cast<int>(bench::argInt(argc, argv, "consumer", 3));
    s.base.highPriority = bench::hasFlag(argc, argv, "--high-priority");
    s.burstMessages = static_cast<uint64_t>(bench::argInt(argc, argv, "burst", 10'000'000));
    s.reps = static_cast<int>(std::max<long long>(1, bench::argInt(argc, argv, "reps", 5)));
    s.pacedMessages = static_cast<uint64_t>(bench::argInt(argc, argv, "paced", 500'000));
    s.intervalTicks = static_cast<uint64_t>(static_cast<double>(intervalNs) * s.ticksPerNs);
    s.base.producerWorkTicks = static_cast<uint64_t>(static_cast<double>(producerWorkNs) * s.ticksPerNs);
    s.base.consumerWorkTicks = static_cast<uint64_t>(static_cast<double>(consumerWorkNs) * s.ticksPerNs);

    std::printf("MiniHFT ring_study: RingBuffer variants, one change at a time (%zu slots, 24-byte messages)\n", kSize);
    bench::printMachine(s.ticksPerNs, overhead);
    std::printf("  threads   producer core %d, consumer core %d, %s priority\n", s.base.producerCore,
                s.base.consumerCore, s.base.highPriority ? "high" : "normal");
    std::printf("  burst     %llu messages back-to-back, median of %d runs\n",
                static_cast<unsigned long long>(s.burstMessages), s.reps);
    std::printf("  paced     %llu messages, one every %llu ns\n", static_cast<unsigned long long>(s.pacedMessages),
                static_cast<unsigned long long>(intervalNs));
    std::printf("  work      producer %lld ns, consumer %lld ns of busy work per message\n\n", producerWorkNs, consumerWorkNs);
    std::printf("%-34s %10s %10s %10s %10s %10s\n", "variant", "burst", "burst", "paced", "paced", "paced");
    std::printf("%-34s %10s %10s %10s %10s %10s\n", "", "ns/msg", "M msg/s", "p50 ns", "p90 ns", "p99 ns");

    study<RingV0FetchAdd<RingMessage, kSize>>("v0  fetch_add (original)", s);
    study<RingV1Store<RingMessage, kSize>>("v1  store(release)", s);
    study<RingV2<RingMessage, kSize>>("v2  + cached indices", s);
    study<RingBuffer<RingMessage, kSize>>("    RingBuffer.hpp (ships v2)", s);
    study<RingV2<RingMessage, kSize, 1, false>>("v2  false sharing (one line)", s);
    study<RingV2<RingMessage, kSize, 4>>("v2  batch publish x4", s);
    study<RingV2<RingMessage, kSize, 16>>("v2  batch publish x16", s);
    study<RingV2<RingMessage, kSize, 64>>("v2  batch publish x64", s);

    if (g_failed) {
        std::printf("\nWARNING: out-of-order messages or negative TSC deltas in at least one run\n");
        return 1;
    }
    return 0;
}
