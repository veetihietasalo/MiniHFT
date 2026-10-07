// Claim: pinThread()'s result can't be ignored. Pinning fails for a core that doesn't exist or
// isn't allowed, and a latency measurement from an unpinned thread means less, so the caller
// has to look. Ignoring it is a warning, and with warnings as errors a build failure.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include "ThreadUtils.hpp"

int main() {
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    return ThreadUtils::pinThread(0) ? 0 : 1;
#else
    ThreadUtils::pinThread(0);
    return 0;
#endif
}
