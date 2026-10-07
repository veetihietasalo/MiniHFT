// Claim: Statistics::mean() refuses integer data (it requires std::floating_point). With
// integers the division would be integer division: the mean of {1, 2} would be 1.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include <vector>

#include "Statistics.hpp"

int main() {
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    const std::vector<double> prices = {1.0, 2.0};
#else
    const std::vector<int> prices = {1, 2};
#endif
    return Statistics::mean(prices) > 1 ? 0 : 1;
}
