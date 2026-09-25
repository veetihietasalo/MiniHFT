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

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

#include "BenchCommon.hpp"
#include "LatencyHistogram.hpp"
#include "RingBuffer.hpp"
#include "ThreadUtils.hpp"
#include "Tsc.hpp"

namespace {

struct Message {
    uint64_t seq;
    uint64_t intendedTsc;
    uint64_t sentTsc;
};

using Ring = RingBuffer<Message, 1024>;

struct Results {
    LatencyHistogram sendToReceive;
    LatencyHistogram intendedToReceive;
    uint64_t outOfOrder = 0;
    uint64_t negativeDeltas = 0; // receive stamped before send: would mean the TSCs are not in sync
    bool producerPinned = false;
    bool consumerPinned = false;
};

uint64_t elapsed(uint64_t from, uint64_t to, uint64_t& negativeDeltas) {
    if (to >= from) return to - from;
    ++negativeDeltas;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (bench::hasFlag(argc, argv, "--help")) {
        std::printf("usage: ring_latency [--producer=2] [--consumer=3] [--messages=1000000] [--warmup=50000] [--interval-ns=1000] [--high-priority]\n");
        return 0;
    }
    const bool highPriority = bench::hasFlag(argc, argv, "--high-priority");
    const int producerCore = static_cast<int>(bench::argInt(argc, argv, "producer", 2));
    const int consumerCore = static_cast<int>(bench::argInt(argc, argv, "consumer", 3));
    const uint64_t messages = static_cast<uint64_t>(bench::argInt(argc, argv, "messages", 1'000'000));
    const uint64_t warmup = static_cast<uint64_t>(bench::argInt(argc, argv, "warmup", 50'000));
    const uint64_t intervalNs = static_cast<uint64_t>(bench::argInt(argc, argv, "interval-ns", 1000));

    const double ticksPerNs = Tsc::calibrateTicksPerNs();
    const uint64_t overhead = Tsc::measureOverheadTicks();
    const uint64_t intervalTicks = static_cast<uint64_t>(static_cast<double>(intervalNs) * ticksPerNs);
    const bool paced = intervalTicks > 0;
    const uint64_t total = warmup + messages;

    auto ring = std::make_unique<Ring>();
    auto results = std::make_unique<Results>();
    std::atomic<bool> consumerReady{false};

    std::thread consumer([&] {
        results->consumerPinned = ThreadUtils::pinThread(consumerCore);
        if (highPriority) ThreadUtils::setHighPriority();
        consumerReady.store(true, std::memory_order_release);

        for (uint64_t i = 0; i < total; ++i) {
            Message* msg = nullptr;
            while ((msg = ring->peek()) == nullptr) {} // pure spin: lowest wake-up latency
            const uint64_t now = Tsc::readOrdered();

            if (msg->seq != i) ++results->outOfOrder;
            if (i >= warmup) {
                results->sendToReceive.record(elapsed(msg->sentTsc, now, results->negativeDeltas));
                if (paced) results->intendedToReceive.record(elapsed(msg->intendedTsc, now, results->negativeDeltas));
            }
            ring->consume();
        }
    });

    std::thread producer([&] {
        results->producerPinned = ThreadUtils::pinThread(producerCore);
        if (highPriority) ThreadUtils::setHighPriority();
        while (!consumerReady.load(std::memory_order_acquire)) {}

        const uint64_t start = Tsc::read() + static_cast<uint64_t>(1e6 * ticksPerNs); // first send 1 ms from now
        for (uint64_t i = 0; i < total; ++i) {
            const uint64_t intended = start + i * intervalTicks;
            if (paced) {
                while (Tsc::read() < intended) Tsc::cpuRelax();
            }
            Message* msg = nullptr;
            while ((msg = ring->claim()) == nullptr) {}
            msg->seq = i;
            msg->intendedTsc = intended;
            msg->sentTsc = Tsc::read();
            ring->publish();
        }
    });

    producer.join();
    consumer.join();

    std::printf("MiniHFT ring_latency: one-way latency through RingBuffer<Message, 1024>\n");
    bench::printMachine(ticksPerNs, overhead);
    std::printf("  threads   producer -> core %d (%s), consumer -> core %d (%s), %s priority\n",
                producerCore, results->producerPinned ? "pinned" : "NOT pinned",
                consumerCore, results->consumerPinned ? "pinned" : "NOT pinned",
                highPriority ? "high" : "normal");
    if (paced) {
        std::printf("  load      1 message every %llu ns, %llu measured after %llu warm-up\n",
                    static_cast<unsigned long long>(intervalNs), static_cast<unsigned long long>(messages),
                    static_cast<unsigned long long>(warmup));
    } else {
        std::printf("  load      back-to-back (burst), %llu measured after %llu warm-up: includes queueing\n",
                    static_cast<unsigned long long>(messages), static_cast<unsigned long long>(warmup));
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
