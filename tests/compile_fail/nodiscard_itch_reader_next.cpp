// Claim: ItchReader::next()'s result can't be ignored. Each call consumes a message from the
// stream, so a dropped span is a message lost. Ignoring it is a warning, and with warnings as
// errors a build failure.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include <cstdio>

#include "ItchParser.hpp"

int main() {
    ItchReader reader(stdin);
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    return reader.next().empty() ? 0 : 1;
#else
    reader.next(); // skips a message without looking at it
    return 0;
#endif
}
