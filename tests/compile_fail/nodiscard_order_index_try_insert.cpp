// Claim: OrderIndex::tryInsert()'s result can't be ignored. It refuses a reference that is still
// live instead of overwriting it (code_quality.md, bug 2); a caller that drops the answer goes
// on as if the second order were indexed. Ignoring it is a warning, and with warnings as
// errors a build failure.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include "OrderIndex.hpp"

int main() {
    OrderIndex<int> index;
    if (!index.tryInsert(7, 100)) return 1;
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    if (!index.tryInsert(7, 200)) return 2; // refused: reference 7 is still live
#else
    index.tryInsert(7, 200); // reference 7 reused while live: refused, and nobody notices
#endif
    return 0;
}
