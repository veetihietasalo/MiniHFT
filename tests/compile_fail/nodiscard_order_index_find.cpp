// Claim: OrderIndex::find()'s result can't be ignored. A lookup whose answer is dropped does
// nothing, so the call is a mistake. Ignoring it is a warning, and with warnings as errors a
// build failure.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include "OrderIndex.hpp"

int main() {
    OrderIndex<int> index;
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    return index.find(7) == nullptr ? 0 : 1;
#else
    index.find(7);
    return 0;
#endif
}
