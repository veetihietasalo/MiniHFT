// One-way latency through RingBuffer between two pinned threads.
//
// The producer stamps each message with the TSC just before publish(); the consumer
// reads the TSC right after peek() sees it. Two views of the same run:
//   send -> receive      how long the hop itself took
//   intended -> receive  measured from when the message was *scheduled* to go out, so
//                        any stall on the producer side (preemption, a full queue)
//                        shows up instead of being hidden (coordinated omission).
//
// Usage: ring_latency [--producer=2] [--consumer=3] [--messages=1000000]
//                     [--warmup=50000] [--interval-ns=1000] [--high-priority]
// --interval-ns=0 sends back-to-back (burst): then the numbers include queueing.
// --high-priority raises both threads (Windows: time-critical; Linux: SCHED_FIFO, needs root)
// so the scheduler is less likely to preempt them.

#include <cstdint>
#include <cstdio>

#include "BenchCommon.hpp"
#include "RingBuffer.hpp"
#include "RingHarness.hpp"
#include "Tsc.hpp"

int main(int argc, char** argv) {
    if (bench::hasFlag(argc, argv, "--help")) {
        std::printf("usage: ring_latency [--producer=2] [--consumer=3] [--messages=1000000] [--warmup=50000] [--interval-ns=1000] [--high-priority]\n");
        return 0;
    }
    const double ticksPerNs = Tsc::calibrateTicksPerNs();
    const uint64_t overhead = Tsc::measureOverheadTicks();
    const uint64_t intervalNs = static_cast<uint64_t>(bench::argInt(argc, argv, "interval-ns", 1000));

    RingRunConfig cfg;
    cfg.producerCore = static_cast<int>(bench::argInt(argc, argv, "producer", 2));
    cfg.consumerCore = static_cast<int>(bench::argInt(argc, argv, "consumer", 3));
    cfg.messages = static_cast<uint64_t>(bench::argInt(argc, argv, "messages", 1'000'000));
    cfg.warmup = static_cast<uint64_t>(bench::argInt(argc, argv, "warmup", 50'000));
    cfg.intervalTicks = static_cast<uint64_t>(static_cast<double>(intervalNs) * ticksPerNs);
    cfg.highPriority = bench::hasFlag(argc, argv, "--high-priority");
    const bool paced = cfg.intervalTicks > 0;

    const auto results = runRing<RingBuffer<RingMessage, 1024>>(cfg, ticksPerNs);

    std::printf("MiniHFT ring_latency: one-way latency through RingBuffer<Message, 1024>\n");
    bench::printMachine(ticksPerNs, overhead);
    std::printf("  threads   producer -> core %d (%s), consumer -> core %d (%s), %s priority\n",
                cfg.producerCore, results->producerPinned ? "pinned" : "NOT pinned",
                cfg.consumerCore, results->consumerPinned ? "pinned" : "NOT pinned",
                cfg.highPriority ? "high" : "normal");
    if (paced) {
        std::printf("  load      1 message every %llu ns, %llu measured after %llu warm-up\n",
                    static_cast<unsigned long long>(intervalNs), static_cast<unsigned long long>(cfg.messages),
                    static_cast<unsigned long long>(cfg.warmup));
    } else {
        std::printf("  load      back-to-back (burst), %llu measured after %llu warm-up: includes queueing\n",
                    static_cast<unsigned long long>(cfg.messages), static_cast<unsigned long long>(cfg.warmup));
    }

    bench::printTableHeader("latency (ns)");
    bench::printTableRow("send -> receive", results->sendToReceive, ticksPerNs);
    if (paced) bench::printTableRow("intended -> receive", results->intendedToReceive, ticksPerNs);

    if (results->outOfOrder != 0 || results->negativeDeltas != 0) {
        std::printf("\nWARNING: %llu out-of-order messages, %llu negative TSC deltas\n",
                    static_cast<unsigned long long>(results->outOfOrder),
                    static_cast<unsigned long long>(results->negativeDeltas));
        return 1;
    }
    return 0;
}
