// Claim: RingBuffer refuses a size of 0, which isn't a power of two either. The check used to
// be `(Size & (Size - 1)) == 0`, which 0 passes, so RingBuffer<T, 0> compiled (with only a
// pedantic warning about the zero-size array) into a queue that is always full and always
// empty. The control uses 1, the smallest power of two, which must still be accepted.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include <cstddef>

#include "RingBuffer.hpp"

#ifdef MINIHFT_COMPILE_FAIL_CONTROL
constexpr size_t kSlots = 1;
#else
constexpr size_t kSlots = 0;
#endif

int main() {
    RingBuffer<int, kSlots> ring;
    return ring.peek() == nullptr ? 0 : 1;
}
