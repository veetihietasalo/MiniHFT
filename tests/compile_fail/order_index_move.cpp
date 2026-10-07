// Claim: an OrderIndex can't be moved. The implicit moves took the slot vector but kept the size,
// mask and shift, so any call on the moved-from index went through an empty table (found by the
// Clang Static Analyzer: a shift by 64 in home()). Keeping a moved-from index usable would put an
// emptiness check in every find(), on the book's hot path, so the moves are deleted instead. The
// control copies, which stays allowed.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include "OrderIndex.hpp"

#ifndef MINIHFT_COMPILE_FAIL_CONTROL
#include <utility> // std::move, for the misuse
#endif

int main() {
    OrderIndex<int> source;
    if (!source.tryInsert(1, 10)) return 1;
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    OrderIndex<int> other(source);
#else
    OrderIndex<int> other(std::move(source));
#endif
    return other.find(1) != nullptr ? 0 : 1;
}
