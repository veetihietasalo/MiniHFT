// Claim: Statistics::movingAverage() refuses integer data (it requires std::floating_point).
// It used to accept it and divide by the window size as unsigned: the window {-2, -4} of
// 64-bit integers averaged to 9,223,372,036,854,775,805.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include <vector>

#include "Statistics.hpp"

int main() {
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    const std::vector<double> prices = {-2.0, -4.0};
#else
    const std::vector<long long> prices = {-2, -4};
#endif
    return Statistics::movingAverage(prices, 2).size() == 1 ? 0 : 1;
}
