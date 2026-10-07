// Claim: Statistics::covariance() refuses integer data (it requires std::floating_point): both
// means and the final division would truncate.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include <stdexcept>
#include <vector>

#include "Statistics.hpp"

int main() {
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    const std::vector<double> x = {1.0, 2.0, 3.0};
    const std::vector<double> y = {2.0, 4.0, 7.0};
#else
    const std::vector<int> x = {1, 2, 3};
    const std::vector<int> y = {2, 4, 7};
#endif
    try {
        return Statistics::covariance(x, y) > 0 ? 0 : 1;
    } catch (const std::invalid_argument&) { // sizes differ, or fewer than 2 values
        return 2;
    }
}
