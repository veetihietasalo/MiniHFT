// Runs the SPSC stress test on RelaxedPublishRingBuffer, whose publish() is deliberately
// relaxed instead of release. What to expect:
//   - ThreadSanitizer build: TSan reports a data race on the slot. In that build this program
//     is registered as a test that passes only if the report appears.
//   - Normal build on x86: usually no damaged messages at all. x86 never lets one store become
//     visible before an earlier one, so the missing release goes unnoticed. ARM gives no such
//     guarantee. See docs/memory_ordering.md.
//
// Usage: relaxed_publish_demo [messages=1000000] [release] [spin] [small]
//   release: run the same queue with publish() a release (RingV0FetchAdd), as a control.
//   spin:    busy-wait instead of yielding when the queue is full or empty.
//   small:   8 slots instead of 1024.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../bench/RingVariants.hpp"
#include "RelaxedPublishRingBuffer.hpp"
#include "SpscStress.hpp"

namespace {

template <template <typename, size_t> class Queue>
StressResult run(uint64_t count, bool small, StressWait wait) {
    return small ? runSpscStress<Queue<StressMessage, 8>>(count, wait)
                 : runSpscStress<Queue<StressMessage, 1024>>(count, wait);
}

} // namespace

int main(int argc, char** argv) {
    const uint64_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
    bool control = false;
    bool small = false;
    StressWait wait = StressWait::yield;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "release") == 0) control = true;
        if (std::strcmp(argv[i], "spin") == 0) wait = StressWait::spin;
        if (std::strcmp(argv[i], "small") == 0) small = true;
    }
    const StressResult r = control ? run<RingV0FetchAdd>(count, small, wait) : run<RelaxedPublishRingBuffer>(count, small, wait);

    std::printf("%s publish: %llu messages, %llu bad sequence, %llu bad payload, %llu bad checksum\n",
                control ? "release" : "relaxed", static_cast<unsigned long long>(r.received),
                static_cast<unsigned long long>(r.badSequence), static_cast<unsigned long long>(r.badPayload),
                static_cast<unsigned long long>(r.badChecksum));
    return r.clean() ? 0 : 1;
}
