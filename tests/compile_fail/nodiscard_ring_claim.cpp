// Claim: RingBuffer::claim()'s result can't be ignored. It returns nullptr when the queue is
// full; a producer that drops the pointer and publishes anyway hands the consumer a slot that
// was never written. Ignoring it is a warning, and with warnings as errors a build failure.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include "RingBuffer.hpp"

int main() {
    RingBuffer<int, 1024> ring;
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    int* slot = ring.claim();
    if (slot == nullptr) return 1; // full
    *slot = 42;
#else
    ring.claim(); // as if a slot were always free
#endif
    ring.publish();
    return 0;
}
