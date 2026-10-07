// Claim: itch::dispatch() refuses a handler that is missing one of the methods the ItchHandler
// concept lists (here onTradingAction), and the error names the concept at the call instead of
// coming from deep inside dispatch().
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include <cstdint>
#include <span>

#include "ItchMessages.hpp"

namespace {

struct Handler {
    void onAdd(uint16_t, uint64_t, const itch::AddOrder&) {}
    void onExecuted(uint16_t, uint64_t, const itch::OrderExecuted&) {}
    void onExecutedWithPrice(uint16_t, uint64_t, const itch::OrderExecutedWithPrice&) {}
    void onCancel(uint16_t, uint64_t, const itch::OrderCancel&) {}
    void onDelete(uint16_t, uint64_t, const itch::OrderDelete&) {}
    void onReplace(uint16_t, uint64_t, const itch::OrderReplace&) {}
    void onTrade(uint16_t, uint64_t, const itch::Trade&) {}
    void onStockDirectory(uint16_t, uint64_t, const itch::StockDirectory&) {}
    void onSystemEvent(uint16_t, uint64_t, const itch::SystemEvent&) {}
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    void onTradingAction(uint16_t, uint64_t, const itch::TradingAction&) {}
#endif
};

} // namespace

int main() {
    Handler handler;
    return itch::dispatch(std::span<const uint8_t>(), handler) ? 1 : 0;
}
