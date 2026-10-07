// Claim: the W03 study's RingV2 refuses a size of 0 through its size check. It used to be
// refused only because Batch (at least 1) can't be at most 0, with an error about the batch.
// The control uses 1, the smallest power of two.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include <cstddef>

#include "../../bench/RingVariants.hpp"

#ifdef MINIHFT_COMPILE_FAIL_CONTROL
constexpr size_t kSlots = 1;
#else
constexpr size_t kSlots = 0;
#endif

int main() {
    RingV2<int, kSlots> ring;
    return ring.peek() == nullptr ? 0 : 1;
}
