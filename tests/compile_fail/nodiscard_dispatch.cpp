// Claim: itch::dispatch()'s result can't be ignored. It returns false, calling nothing, for a
// message whose length doesn't match its type; a caller that drops the answer loses that
// message without a trace. Ignoring it is a warning, and with warnings as errors a build
// failure. A caller that knows every message is well formed says so with a (void) cast.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include <cstdint>
#include <memory>
#include <span>

#include "ItchBook.hpp"
#include "ItchMessages.hpp"

int main() {
    auto builder = std::make_unique<ItchBookBuilder>(1024);
    const std::span<const uint8_t> empty; // not a message, so dispatch() refuses it
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    return itch::dispatch(empty, *builder) ? 1 : 0;
#else
    itch::dispatch(empty, *builder); // the refusal goes unnoticed
    return 0;
#endif
}
