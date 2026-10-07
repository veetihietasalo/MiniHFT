// Claim: a TopOfBookListener's onTopOfBook must return void. The builder ignores whatever the
// listener returns, so a listener that answers (here: whether it wants to trade) would be
// talking to nobody. The concept's `-> std::same_as<void>` refuses it.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include <memory>

#include "ItchBook.hpp"

namespace {

struct TradeSignal {
    bool wantsToTrade = false;
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    void onTopOfBook(const TopOfBook& top) { wantsToTrade = top.askPrice != 0 && top.askPrice < 100'0000u; }
#else
    bool onTopOfBook(const TopOfBook& top) { return top.askPrice != 0 && top.askPrice < 100'0000u; }
#endif
};

} // namespace

int main() {
    auto builder = std::make_unique<BasicItchBookBuilder<TradeSignal>>(1024);
    return builder->listener().wantsToTrade ? 1 : 0;
}
