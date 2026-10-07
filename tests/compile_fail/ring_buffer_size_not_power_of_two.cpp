// Claim: RingBuffer refuses a size that isn't a power of two. It maps an index to a slot with
// `index & (Size - 1)`, which wraps correctly only for a power of two: with 1000 slots, index
// 1000 would land in slot 992 instead of slot 0.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include <cstddef>

#include "RingBuffer.hpp"

#ifdef MINIHFT_COMPILE_FAIL_CONTROL
constexpr size_t kSlots = 1024;
#else
constexpr size_t kSlots = 1000;
#endif

int main() {
    RingBuffer<int, kSlots> ring;
    return ring.peek() == nullptr ? 0 : 1;
}
