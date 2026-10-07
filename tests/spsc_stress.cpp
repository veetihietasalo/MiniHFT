// Schedule stress for every single-producer / single-consumer hand-off in the project:
// RingBuffer, the W03 study variants (bench/RingVariants.hpp), KernelBypass, and three
// deliberately broken queues (relaxed publish, relaxed consume, off-by-one full check).
//
// ThreadSanitizer only sees the interleavings that actually happen. This program runs one
// queue under one schedule (tests/SpscStress.hpp):
//   yield    yield whenever the queue is full or empty (what the unit tests do)
//   tight    busy-spin on a full or empty queue
//   random   seeded random spins, yields and stalls between the steps of every hand-off
// and the "-tiny" queues have 2 or 4 slots, so they wrap, fill and empty constantly.
//
// Usage: spsc_stress <queue> <yield|tight|random> [messages=20000] [seed=random]
//        spsc_stress --list
// The first line printed is the command that repeats the run with the same seed. The threads
// still interleave differently each time, but the delays are the same.
// Exit code 0 if every message arrived intact and in order, 1 if not, 2 on a usage error.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <thread>

#include "../bench/RingVariants.hpp"
#include "KernelBypass.hpp"
#include "OffByOneRingBuffer.hpp"
#include "RelaxedConsumeRingBuffer.hpp"
#include "RelaxedPublishRingBuffer.hpp"
#include "RingBuffer.hpp"
#include "SpscStress.hpp"

namespace {

using StressFn = StressResult (*)(uint64_t, StressSchedule, uint64_t);

template <typename Queue>
StressResult stress(uint64_t count, StressSchedule schedule, uint64_t seed) {
    return runSpscStress<Queue>(count, schedule, seed);
}

// KernelBypass hands over one descriptor at a time through its `ready` flag rather than through
// claim/publish/peek/consume, so it gets its own loop with the same delays. Its ring has a fixed
// 1024 descriptors: anything past 1024 packets reuses them.
StressResult stressKernelBypass(uint64_t count, StressSchedule schedule, uint64_t seed) {
    auto nic = std::make_unique<KernelBypass>();
    std::thread wire([&] {
        SchedulePerturber perturb(schedule, seed ^ 0x5bd1e995ULL);
        for (uint64_t i = 0; i < count; ++i) {
            perturb.step();
            while (!nic->nicReceive(static_cast<uint32_t>(i), static_cast<size_t>(i))) perturb.blocked();
        }
    });

    StressResult result;
    SchedulePerturber perturb(schedule, seed ^ 0xc2b2ae35ULL);
    for (uint64_t expected = 0; expected < count; ++expected) {
        perturb.step();
        uint32_t packet = 0;
        while (!nic->poll(packet)) perturb.blocked();
        ++result.received;
        if (packet != static_cast<uint32_t>(expected)) ++result.badSequence;
    }
    wire.join();
    return result;
}

struct QueueEntry {
    const char* name;
    StressFn run;
    const char* description;
};

constexpr QueueEntry kQueues[] = {
    {"ring", stress<RingBuffer<StressMessage, 1024>>, "RingBuffer, 1024 slots"},
    {"ring-tiny", stress<RingBuffer<StressMessage, 2>>, "RingBuffer, 2 slots"},
    {"v0", stress<RingV0FetchAdd<StressMessage, 1024>>, "RingV0FetchAdd, 1024 slots"},
    {"v0-tiny", stress<RingV0FetchAdd<StressMessage, 2>>, "RingV0FetchAdd, 2 slots"},
    {"v1", stress<RingV1Store<StressMessage, 1024>>, "RingV1Store, 1024 slots"},
    {"v1-tiny", stress<RingV1Store<StressMessage, 2>>, "RingV1Store, 2 slots"},
    {"v2", stress<RingV2<StressMessage, 1024>>, "RingV2, 1024 slots"},
    {"v2-one-line", stress<RingV2<StressMessage, 1024, 1, false>>, "RingV2, both indices on one cache line"},
    {"v2-batch4", stress<RingV2<StressMessage, 1024, 4>>, "RingV2, 1024 slots, publishes every 4th message"},
    {"v2-tiny-batch", stress<RingV2<StressMessage, 4, 4>>, "RingV2, 4 slots, batch = size"},
    {"kernel-bypass", stressKernelBypass, "KernelBypass descriptor ring, 1024 descriptors"},
    // Deliberately broken: only meaningful under ThreadSanitizer, which should report a race.
    {"relaxed-publish", stress<RelaxedPublishRingBuffer<StressMessage, 1024>>, "BROKEN: publish() relaxed, 1024 slots"},
    {"relaxed-publish-tiny", stress<RelaxedPublishRingBuffer<StressMessage, 2>>, "BROKEN: publish() relaxed, 2 slots"},
    {"relaxed-consume", stress<RelaxedConsumeRingBuffer<StressMessage, 1024>>, "BROKEN: consume() relaxed, 1024 slots"},
    {"relaxed-consume-tiny", stress<RelaxedConsumeRingBuffer<StressMessage, 2>>, "BROKEN: consume() relaxed, 2 slots"},
    {"off-by-one", stress<OffByOneRingBuffer<StressMessage, 1024>>, "BROKEN: claim() accepts 1025 messages in 1024 slots"},
    {"off-by-one-tiny", stress<OffByOneRingBuffer<StressMessage, 2>>, "BROKEN: claim() accepts 3 messages in 2 slots"},
};

const QueueEntry* findQueue(const char* name) {
    for (const QueueEntry& q : kQueues) {
        if (std::strcmp(q.name, name) == 0) return &q;
    }
    return nullptr;
}

bool parseSchedule(const char* name, StressSchedule& out) {
    if (std::strcmp(name, "yield") == 0) {
        out = StressSchedule::YieldWhenBlocked;
    } else if (std::strcmp(name, "tight") == 0) {
        out = StressSchedule::Tight;
    } else if (std::strcmp(name, "random") == 0) {
        out = StressSchedule::Random;
    } else {
        return false;
    }
    return true;
}

void printUsage() {
    std::printf("usage: spsc_stress <queue> <yield|tight|random> [messages=20000] [seed=random]\n"
                "       spsc_stress --list\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const QueueEntry& q : kQueues) std::printf("%-22s %s\n", q.name, q.description);
        return 0;
    }
    StressSchedule schedule = StressSchedule::YieldWhenBlocked;
    const QueueEntry* queue = argc >= 3 ? findQueue(argv[1]) : nullptr;
    if (queue == nullptr || !parseSchedule(argv[2], schedule)) {
        printUsage();
        return 2;
    }
    const uint64_t count = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 20'000;
    const uint64_t seed = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : static_cast<uint64_t>(std::random_device{}());

    // Printed (and flushed) first, so it comes before any ThreadSanitizer report.
    std::printf("spsc_stress %s %s %llu %llu   (%s)\n", queue->name, argv[2], static_cast<unsigned long long>(count),
                static_cast<unsigned long long>(seed), queue->description);
    std::fflush(stdout);

    const StressResult r = queue->run(count, schedule, seed);
    std::printf("%llu of %llu messages received, %llu bad sequence, %llu bad payload, %llu bad checksum\n",
                static_cast<unsigned long long>(r.received), static_cast<unsigned long long>(count),
                static_cast<unsigned long long>(r.badSequence), static_cast<unsigned long long>(r.badPayload),
                static_cast<unsigned long long>(r.badChecksum));
    return (r.received == count && r.clean()) ? 0 : 1;
}
