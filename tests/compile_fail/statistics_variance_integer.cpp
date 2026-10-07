// Claim: Statistics::variance() refuses integer data (it requires std::floating_point): its
// mean and its final division would both truncate.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include <vector>

#include "Statistics.hpp"

int main() {
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    const std::vector<double> returns = {1.0, 2.0, 4.0};
#else
    const std::vector<long long> returns = {1, 2, 4};
#endif
    return Statistics::variance(returns) > 0 ? 0 : 1;
}
