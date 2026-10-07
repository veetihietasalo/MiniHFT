// Claim: KernelBypass::poll()'s result can't be ignored. It returns false when nothing has
// arrived, and then it hasn't written its output. Ignoring it is a warning, and with warnings
// as errors a build failure.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include <cstdint>

#include "KernelBypass.hpp"

int main() {
    KernelBypass nic;
    uint32_t packet = 0;
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    if (!nic.poll(packet)) return 1; // nothing received
#else
    nic.poll(packet); // packet is used below whether or not one arrived
#endif
    return packet == 0 ? 0 : 2;
}
