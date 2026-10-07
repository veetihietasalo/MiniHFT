// Claim: KernelBypass::nicReceive()'s result can't be ignored. It returns false when the CPU
// hasn't polled the slot yet and the packet is dropped, as a real NIC drops it. Ignoring it is
// a warning, and with warnings as errors a build failure.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include "KernelBypass.hpp"

int main() {
    KernelBypass nic;
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    if (!nic.nicReceive(1, 0)) return 1; // ring full: the packet was dropped
#else
    nic.nicReceive(1, 0); // a dropped packet would go unnoticed
#endif
    return 0;
}
