// Writes a small ITCH 5.0 file, market_data.itch, in the format of NASDAQ's sample files:
// each message is preceded by its length as a 2-byte big-endian integer.
// itch_test replays it into an order book.
//
// Usage: gen_itch [--orders=N] [--out=market_data.itch]
//   --orders=N  also N rounds of order flow during the session: an add, and once 100 orders are
//               resting, the oldest leaves (deleted or fully executed, alternately). Every event
//               refers to a live order, so a book builder must report no unknown references. A
//               long, consistent feed for mold_send and the networking measurements.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ItchMessages.hpp"

int main(int argc, char** argv) {
    const char* path = "market_data.itch";
    unsigned long long orders = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--orders=", 9) == 0) orders = std::strtoull(argv[i] + 9, nullptr, 10);
        if (std::strncmp(argv[i], "--out=", 6) == 0) path = argv[i] + 6;
    }
    std::FILE* file = std::fopen(path, "wb");
    if (!file) {
        std::fprintf(stderr, "Failed to create %s\n", path);
        return 1;
    }

    unsigned long long count = 0;
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

    // Order flow away from the inside, so the hand-written book above keeps its best prices.
    constexpr uint64_t kWindow = 100;
    constexpr uint64_t kFirstRef = 1'000'000;
    for (uint64_t i = 0; i < orders; ++i) {
        const uint64_t t = ts + 100 + i;
        const bool bid = i % 2 == 0;
        const auto ticks = static_cast<uint32_t>(10 + i % 40); // 10-49 ticks from 150.00
        const uint32_t price = bid ? 1'500'000 - ticks * 100 : 1'500'000 + ticks * 100;
        emit([&](uint8_t* m) { return itch::encodeAddOrder(m, kGoogl, t, kFirstRef + i, bid ? 'B' : 'S', 100, "GOOGL   ", price); });
        if (i < kWindow) continue;
        const uint64_t oldest = kFirstRef + i - kWindow;
        if (i % 2 == 0) {
            emit([&](uint8_t* m) { return itch::encodeOrderDelete(m, kGoogl, t, oldest); });
        } else {
            emit([&](uint8_t* m) { return itch::encodeOrderExecuted(m, kGoogl, t, oldest, 100, i); });
        }
    }

    ts = 16 * kHour;
    emit([&](uint8_t* m) { return itch::encodeSystemEvent(m, ts, 'M'); });
    emit([&](uint8_t* m) { return itch::encodeSystemEvent(m, ts + 1, 'C'); });

    std::fclose(file);
    std::printf("Wrote %llu messages to %s\n", count, path);
    return 0;
}
