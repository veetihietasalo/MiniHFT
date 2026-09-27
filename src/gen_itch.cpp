// Writes a small ITCH 5.0 file, market_data.itch, in the format of NASDAQ's sample files:
// each message is preceded by its length as a 2-byte big-endian integer.
// itch_test replays it into an order book.

#include <cstdint>
#include <cstdio>

#include "ItchMessages.hpp"

int main() {
    std::FILE* file = std::fopen("market_data.itch", "wb");
    if (!file) {
        std::fprintf(stderr, "Failed to create market_data.itch\n");
        return 1;
    }

    int count = 0;
    auto emit = [&](auto encode) {
        uint8_t message[64] = {};
        const size_t n = encode(message);
        uint8_t prefix[2];
        itch::putBe16(prefix, static_cast<uint16_t>(n));
        std::fwrite(prefix, 1, 2, file);
        std::fwrite(message, 1, n, file);
        ++count;
    };

    constexpr uint16_t kGoogl = 1;
    constexpr uint64_t kHour = 3'600'000'000'000ULL;
    uint64_t ts = 4 * kHour;
    emit([&](uint8_t* m) { return itch::encodeSystemEvent(m, ts, 'O'); });
    emit([&](uint8_t* m) { return itch::encodeStockDirectory(m, kGoogl, ts, "GOOGL   "); });

    ts = 9 * kHour + kHour / 2; // 09:30
    emit([&](uint8_t* m) { return itch::encodeSystemEvent(m, ts, 'Q'); });

    // Ten resting orders around $150: bids at 149.99, 149.98, ... and asks at 150.01, 150.02, ...
    for (uint32_t i = 0; i < 10; ++i) {
        const bool bid = i % 2 == 0;
        const uint32_t price = bid ? 1'499'900 - (i / 2) * 100 : 1'500'100 + (i / 2) * 100;
        emit([&](uint8_t* m) { return itch::encodeAddOrder(m, kGoogl, ts + i, 1000 + i, bid ? 'B' : 'S', 100, "GOOGL   ", price); });
    }
    emit([&](uint8_t* m) { return itch::encodeOrderExecuted(m, kGoogl, ts + 20, 1000, 60, 1); });           // best bid partly filled
    emit([&](uint8_t* m) { return itch::encodeOrderCancel(m, kGoogl, ts + 21, 1001, 20); });                // best ask shrinks
    emit([&](uint8_t* m) { return itch::encodeOrderReplace(m, kGoogl, ts + 22, 1002, 2002, 150, 1'500'000); }); // bid moves up to 150.00
    emit([&](uint8_t* m) { return itch::encodeOrderDelete(m, kGoogl, ts + 23, 1003); });
    emit([&](uint8_t* m) { return itch::encodeTrade(m, kGoogl, ts + 24, 0, 'B', 50, "GOOGL   ", 1'500'050, 2); });

    ts = 16 * kHour;
    emit([&](uint8_t* m) { return itch::encodeSystemEvent(m, ts, 'M'); });
    emit([&](uint8_t* m) { return itch::encodeSystemEvent(m, ts + 1, 'C'); });

    std::fclose(file);
    std::printf("Wrote %d messages to market_data.itch\n", count);
    return 0;
}
