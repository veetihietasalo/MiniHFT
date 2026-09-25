// Runs the SPSC stress test on RelaxedPublishRingBuffer, whose publish() is deliberately
// relaxed instead of release. What to expect:
//   - ThreadSanitizer build: TSan reports a data race on the slot. In that build this program
//     is registered as a test that passes only if the report appears.
//   - Normal build on x86: usually no damaged messages at all. x86 never lets one store become
//     visible before an earlier one, so the missing release goes unnoticed. ARM gives no such
//     guarantee. See docs/memory_ordering.md.
//
// Usage: relaxed_publish_demo [messages=1000000]

#include <cstdio>
#include <cstdlib>

#include "RelaxedPublishRingBuffer.hpp"
#include "SpscStress.hpp"

int main(int argc, char** argv) {
    const uint64_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
    const StressResult r = runSpscStress<RelaxedPublishRingBuffer<StressMessage, 1024>>(count);

    std::printf("relaxed publish: %llu messages, %llu bad sequence, %llu bad payload, %llu bad checksum\n",
                static_cast<unsigned long long>(r.received), static_cast<unsigned long long>(r.badSequence),
                static_cast<unsigned long long>(r.badPayload), static_cast<unsigned long long>(r.badChecksum));
    return r.clean() ? 0 : 1;
}
