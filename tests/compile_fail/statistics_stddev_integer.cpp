// Claim: Statistics::stdDev() refuses integer data (it requires std::floating_point), so an
// integer volatility can't reach a strategy.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include <vector>

#include "Statistics.hpp"

int main() {
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    const std::vector<double> ticks = {3.0, 5.0, 9.0};
#else
    const std::vector<unsigned> ticks = {3, 5, 9};
#endif
    return Statistics::stdDev(ticks) > 0 ? 0 : 1;
}
