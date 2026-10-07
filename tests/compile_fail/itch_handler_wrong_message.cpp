// Claim: itch::dispatch() refuses a handler whose method takes the wrong message type. Here
// onExecuted takes OrderExecutedWithPrice, as if copied from the line below it; the message
// structs don't convert into each other, so the ItchHandler concept rejects it.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include <cstdint>
#include <span>

#include "ItchMessages.hpp"

namespace {

struct Handler {
    void onAdd(uint16_t, uint64_t, const itch::AddOrder&) {}
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    void onExecuted(uint16_t, uint64_t, const itch::OrderExecuted&) {}
#else
    void onExecuted(uint16_t, uint64_t, const itch::OrderExecutedWithPrice&) {}
#endif
    void onExecutedWithPrice(uint16_t, uint64_t, const itch::OrderExecutedWithPrice&) {}
    void onCancel(uint16_t, uint64_t, const itch::OrderCancel&) {}
    void onDelete(uint16_t, uint64_t, const itch::OrderDelete&) {}
    void onReplace(uint16_t, uint64_t, const itch::OrderReplace&) {}
    void onTrade(uint16_t, uint64_t, const itch::Trade&) {}
    void onStockDirectory(uint16_t, uint64_t, const itch::StockDirectory&) {}
    void onSystemEvent(uint16_t, uint64_t, const itch::SystemEvent&) {}
    void onTradingAction(uint16_t, uint64_t, const itch::TradingAction&) {}
};

} // namespace

int main() {
    Handler handler;
    return itch::dispatch(std::span<const uint8_t>(), handler) ? 1 : 0;
}
