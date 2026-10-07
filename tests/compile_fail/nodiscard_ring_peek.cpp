// Claim: RingBuffer::peek()'s result can't be ignored. It returns nullptr when the queue is
// empty; a consumer that drops the pointer and consumes anyway moves the tail past a message
// that isn't there. Ignoring it is a warning, and with warnings as errors a build failure.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include "RingBuffer.hpp"

int main() {
    RingBuffer<int, 1024> ring;
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    if (ring.peek() == nullptr) return 1; // empty: nothing to consume
#else
    ring.peek(); // as if a message were always there
#endif
    ring.consume();
    return 0;
}
