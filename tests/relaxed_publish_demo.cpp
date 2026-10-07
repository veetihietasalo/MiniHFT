// Runs the SPSC stress test on RelaxedPublishRingBuffer, whose publish() is deliberately
// relaxed instead of release. What to expect:
//   - ThreadSanitizer build: TSan reports a data race on the slot. In that build this program
//     is registered as a test that passes only if the report appears.
//   - Normal build on x86: usually no damaged messages at all. x86 never lets one store become
//     visible before an earlier one, so the missing release goes unnoticed.
//   - Normal build on ARM: stale and torn messages, in every run with `small`. On AArch64 this
//     program is registered as a test that passes only if some arrive. See docs/memory_ordering.md.
//
// Usage: relaxed_publish_demo [messages=1000000] [release] [small] [until-damaged]
//   release:       publish() is a release (RingV0FetchAdd): the control.
//   small:         8 slots instead of 1024. The consumer has just read every slot the producer
//                  writes next, and on ARM that makes the reordering far more frequent.
//   until-damaged: run in rounds of 10M messages, stopping after the first round with damage.

#include <algorithm>
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
StressResult run(uint64_t count, bool small) {
    return small ? runSpscStress<Queue<StressMessage, 8>>(count) : runSpscStress<Queue<StressMessage, 1024>>(count);
}

} // namespace

int main(int argc, char** argv) {
    const uint64_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
    bool control = false;
    bool small = false;
    bool untilDamaged = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "release") == 0) control = true;
        if (std::strcmp(argv[i], "small") == 0) small = true;
        if (std::strcmp(argv[i], "until-damaged") == 0) untilDamaged = true;
    }

    StressResult total;
    for (uint64_t remaining = count; remaining > 0 && (!untilDamaged || total.clean());) {
        const uint64_t n = untilDamaged ? std::min<uint64_t>(remaining, 10'000'000) : remaining;
        const StressResult r = control ? run<RingV0FetchAdd>(n, small) : run<RelaxedPublishRingBuffer>(n, small);
        total.received += r.received;
        total.badSequence += r.badSequence;
        total.badPayload += r.badPayload;
        total.badChecksum += r.badChecksum;
        remaining -= n;
    }

    std::printf("%s publish: %llu messages, %llu bad sequence, %llu bad payload, %llu bad checksum\n",
                control ? "release" : "relaxed", static_cast<unsigned long long>(total.received),
                static_cast<unsigned long long>(total.badSequence), static_cast<unsigned long long>(total.badPayload),
                static_cast<unsigned long long>(total.badChecksum));
    return total.clean() ? 0 : 1;
}
