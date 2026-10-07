// Claim: RingV2 refuses a batch larger than the queue. The producer publishes once per Batch
// messages, but claim() fails once Size messages are waiting, so with Batch > Size the first
// batch is never published: the producer spins on a full queue and the consumer sees nothing.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include <cstddef>

#include "../../bench/RingVariants.hpp"

#ifdef MINIHFT_COMPILE_FAIL_CONTROL
constexpr size_t kBatch = 8;
#else
constexpr size_t kBatch = 16;
#endif

int main() {
    RingV2<int, 8, kBatch> ring;
    return ring.peek() == nullptr ? 0 : 1;
}
