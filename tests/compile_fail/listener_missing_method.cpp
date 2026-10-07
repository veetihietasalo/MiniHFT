// Claim: BasicItchBookBuilder<Listener> refuses a Listener that isn't a TopOfBookListener.
// Here the method has another name, so the builder would have nothing to call on a quote
// change; the concept stops it at the builder's template argument.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include <cstdint>
#include <memory>

#include "ItchBook.hpp"

namespace {

struct QuoteLogger {
    uint64_t quotes = 0;
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    void onTopOfBook(const TopOfBook&) { ++quotes; }
#else
    void onQuote(const TopOfBook&) { ++quotes; }
#endif
};

} // namespace

int main() {
    auto builder = std::make_unique<BasicItchBookBuilder<QuoteLogger>>(1024);
    return builder->listener().quotes == 0 ? 0 : 1;
}
