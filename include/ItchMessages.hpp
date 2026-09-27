#pragma once

// NASDAQ TotalView-ITCH 5.0 messages: layouts, decoding and encoding.
//
// Every message starts with the same 11 bytes:
//   [0] type   [1..2] stock locate   [3..4] tracking number   [5..10] timestamp
// The timestamp is 6 bytes: nanoseconds since midnight. All integers are big-endian. Prices are
// unsigned 32-bit with 4 implied decimals (1234500 = $123.45).
//
// Fields are read byte by byte at their spec offsets instead of casting the buffer to a packed
// struct. That avoids alignment and aliasing undefined behaviour, and compilers turn each read
// into one load plus a byte swap.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace itch {

    // ---- big-endian helpers ----
    inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
    inline uint32_t be32(const uint8_t* p) {
        return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | uint32_t{p[3]};
    }
    inline uint64_t be48(const uint8_t* p) { return uint64_t{be16(p)} << 32 | be32(p + 2); }
    inline uint64_t be64(const uint8_t* p) { return uint64_t{be32(p)} << 32 | be32(p + 4); }

    inline void putBe16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
    inline void putBe32(uint8_t* p, uint32_t v) { putBe16(p, uint16_t(v >> 16)); putBe16(p + 2, uint16_t(v)); }
    inline void putBe48(uint8_t* p, uint64_t v) { putBe16(p, uint16_t(v >> 32)); putBe32(p + 2, uint32_t(v)); }
    inline void putBe64(uint8_t* p, uint64_t v) { putBe32(p, uint32_t(v >> 32)); putBe32(p + 4, uint32_t(v)); }

    // Length in bytes of each message type, without the 2-byte length prefix used in files.
    // 0 means the type is unknown.
    constexpr size_t messageLength(char type) {
        switch (type) {
            case 'S': return 12; // system event
            case 'R': return 39; // stock directory
            case 'H': return 25; // stock trading action
            case 'Y': return 20; // Reg SHO restriction
            case 'L': return 26; // market participant position
            case 'V': return 35; // MWCB decline level
            case 'W': return 12; // MWCB status
            case 'K': return 28; // IPO quoting period update
            case 'J': return 35; // LULD auction collar
            case 'h': return 21; // operational halt
            case 'A': return 36; // add order
            case 'F': return 40; // add order with MPID attribution
            case 'E': return 31; // order executed
            case 'C': return 36; // order executed with price
            case 'X': return 23; // order cancel (partial)
            case 'D': return 19; // order delete
            case 'U': return 35; // order replace
            case 'P': return 44; // trade (non-displayed order)
            case 'Q': return 40; // cross trade
            case 'B': return 19; // broken trade
            case 'I': return 50; // net order imbalance
            case 'N': return 20; // retail price improvement indicator
            case 'O': return 48; // direct listing with capital raise
            default: return 0;
        }
    }

    // ---- decoded messages ----
    struct Header {
        char type;
        uint16_t locate;
        uint16_t tracking;
        uint64_t timestamp;
    };
    struct AddOrder { uint64_t ref; char side; uint32_t shares; char stock[8]; uint32_t price; };             // 'A', 'F'
    struct OrderExecuted { uint64_t ref; uint32_t shares; uint64_t match; };                                    // 'E'
    struct OrderExecutedWithPrice { uint64_t ref; uint32_t shares; uint64_t match; char printable; uint32_t price; }; // 'C'
    struct OrderCancel { uint64_t ref; uint32_t shares; };                                                      // 'X'
    struct OrderDelete { uint64_t ref; };                                                                        // 'D'
    struct OrderReplace { uint64_t oldRef; uint64_t newRef; uint32_t shares; uint32_t price; };                 // 'U'
    struct Trade { uint64_t ref; char side; uint32_t shares; char stock[8]; uint32_t price; uint64_t match; };   // 'P'
    struct StockDirectory { char stock[8]; };                                                                    // 'R'
    struct SystemEvent { char code; };                                                                           // 'S'
    struct TradingAction { char stock[8]; char state; char reason[4]; };                                          // 'H'
    // TradingAction::state: 'T' trading, 'H' halted, 'P' paused (e.g. limit up-limit down), 'Q' quotation only

    inline Header decodeHeader(const uint8_t* m) { return {char(m[0]), be16(m + 1), be16(m + 3), be48(m + 5)}; }

    inline AddOrder decodeAddOrder(const uint8_t* m) {
        AddOrder a{be64(m + 11), char(m[19]), be32(m + 20), {}, be32(m + 32)};
        std::memcpy(a.stock, m + 24, 8);
        return a;
    }
    inline OrderExecuted decodeOrderExecuted(const uint8_t* m) { return {be64(m + 11), be32(m + 19), be64(m + 23)}; }
    inline OrderExecutedWithPrice decodeOrderExecutedWithPrice(const uint8_t* m) {
        return {be64(m + 11), be32(m + 19), be64(m + 23), char(m[31]), be32(m + 32)};
    }
    inline OrderCancel decodeOrderCancel(const uint8_t* m) { return {be64(m + 11), be32(m + 19)}; }
    inline OrderDelete decodeOrderDelete(const uint8_t* m) { return {be64(m + 11)}; }
    inline OrderReplace decodeOrderReplace(const uint8_t* m) { return {be64(m + 11), be64(m + 19), be32(m + 27), be32(m + 31)}; }
    inline Trade decodeTrade(const uint8_t* m) {
        Trade t{be64(m + 11), char(m[19]), be32(m + 20), {}, be32(m + 32), be64(m + 36)};
        std::memcpy(t.stock, m + 24, 8);
        return t;
    }
    inline StockDirectory decodeStockDirectory(const uint8_t* m) {
        StockDirectory d{};
        std::memcpy(d.stock, m + 11, 8);
        return d;
    }
    inline SystemEvent decodeSystemEvent(const uint8_t* m) { return {char(m[11])}; }
    inline TradingAction decodeTradingAction(const uint8_t* m) {
        TradingAction a{};
        std::memcpy(a.stock, m + 11, 8);
        a.state = char(m[19]);
        std::memcpy(a.reason, m + 21, 4);
        return a;
    }

    // ---- encoding (gen_itch and tests) ----
    // Each writes one message at `out` (sized messageLength(type)) and returns its length.
    inline size_t encodeHeader(uint8_t* out, char type, uint16_t locate, uint64_t timestamp, uint16_t tracking = 0) {
        std::memset(out, 0, messageLength(type));
        out[0] = uint8_t(type);
        putBe16(out + 1, locate);
        putBe16(out + 3, tracking);
        putBe48(out + 5, timestamp);
        return messageLength(type);
    }
    inline size_t encodeAddOrder(uint8_t* out, uint16_t locate, uint64_t ts, uint64_t ref, char side, uint32_t shares,
                                 const char* stock8, uint32_t price) {
        const size_t n = encodeHeader(out, 'A', locate, ts);
        putBe64(out + 11, ref);
        out[19] = uint8_t(side);
        putBe32(out + 20, shares);
        std::memcpy(out + 24, stock8, 8);
        putBe32(out + 32, price);
        return n;
    }
    inline size_t encodeOrderExecuted(uint8_t* out, uint16_t locate, uint64_t ts, uint64_t ref, uint32_t shares, uint64_t match) {
        const size_t n = encodeHeader(out, 'E', locate, ts);
        putBe64(out + 11, ref);
        putBe32(out + 19, shares);
        putBe64(out + 23, match);
        return n;
    }
    inline size_t encodeOrderExecutedWithPrice(uint8_t* out, uint16_t locate, uint64_t ts, uint64_t ref, uint32_t shares,
                                               uint64_t match, char printable, uint32_t price) {
        const size_t n = encodeHeader(out, 'C', locate, ts);
        putBe64(out + 11, ref);
        putBe32(out + 19, shares);
        putBe64(out + 23, match);
        out[31] = uint8_t(printable);
        putBe32(out + 32, price);
        return n;
    }
    inline size_t encodeOrderCancel(uint8_t* out, uint16_t locate, uint64_t ts, uint64_t ref, uint32_t shares) {
        const size_t n = encodeHeader(out, 'X', locate, ts);
        putBe64(out + 11, ref);
        putBe32(out + 19, shares);
        return n;
    }
    inline size_t encodeOrderDelete(uint8_t* out, uint16_t locate, uint64_t ts, uint64_t ref) {
        const size_t n = encodeHeader(out, 'D', locate, ts);
        putBe64(out + 11, ref);
        return n;
    }
    inline size_t encodeOrderReplace(uint8_t* out, uint16_t locate, uint64_t ts, uint64_t oldRef, uint64_t newRef,
                                     uint32_t shares, uint32_t price) {
        const size_t n = encodeHeader(out, 'U', locate, ts);
        putBe64(out + 11, oldRef);
        putBe64(out + 19, newRef);
        putBe32(out + 27, shares);
        putBe32(out + 31, price);
        return n;
    }
    inline size_t encodeTrade(uint8_t* out, uint16_t locate, uint64_t ts, uint64_t ref, char side, uint32_t shares,
                              const char* stock8, uint32_t price, uint64_t match) {
        const size_t n = encodeHeader(out, 'P', locate, ts);
        putBe64(out + 11, ref);
        out[19] = uint8_t(side);
        putBe32(out + 20, shares);
        std::memcpy(out + 24, stock8, 8);
        putBe32(out + 32, price);
        putBe64(out + 36, match);
        return n;
    }
    inline size_t encodeStockDirectory(uint8_t* out, uint16_t locate, uint64_t ts, const char* stock8) {
        const size_t n = encodeHeader(out, 'R', locate, ts);
        std::memcpy(out + 11, stock8, 8);
        return n;
    }
    inline size_t encodeSystemEvent(uint8_t* out, uint64_t ts, char code) {
        const size_t n = encodeHeader(out, 'S', 0, ts);
        out[11] = uint8_t(code);
        return n;
    }
    inline size_t encodeTradingAction(uint8_t* out, uint16_t locate, uint64_t ts, const char* stock8, char state,
                                      const char* reason4) {
        const size_t n = encodeHeader(out, 'H', locate, ts);
        std::memcpy(out + 11, stock8, 8);
        out[19] = uint8_t(state);
        std::memcpy(out + 21, reason4, 4);
        return n;
    }

    // Calls the handler for each message type that matters to an order book or a symbol map.
    // Every other type is ignored. The handler needs onAdd, onExecuted, onExecutedWithPrice,
    // onCancel, onDelete, onReplace, onTrade, onStockDirectory, onSystemEvent and
    // onTradingAction; each takes (locate, timestamp, decoded message).
    template <typename Handler>
    void dispatch(const uint8_t* m, Handler& h) {
        const uint16_t locate = be16(m + 1);
        const uint64_t ts = be48(m + 5);
        switch (char(m[0])) {
            case 'A':
            case 'F': h.onAdd(locate, ts, decodeAddOrder(m)); break;
            case 'E': h.onExecuted(locate, ts, decodeOrderExecuted(m)); break;
            case 'C': h.onExecutedWithPrice(locate, ts, decodeOrderExecutedWithPrice(m)); break;
            case 'X': h.onCancel(locate, ts, decodeOrderCancel(m)); break;
            case 'D': h.onDelete(locate, ts, decodeOrderDelete(m)); break;
            case 'U': h.onReplace(locate, ts, decodeOrderReplace(m)); break;
            case 'P': h.onTrade(locate, ts, decodeTrade(m)); break;
            case 'R': h.onStockDirectory(locate, ts, decodeStockDirectory(m)); break;
            case 'S': h.onSystemEvent(locate, ts, decodeSystemEvent(m)); break;
            case 'H': h.onTradingAction(locate, ts, decodeTradingAction(m)); break;
            default: break;
        }
    }
}
