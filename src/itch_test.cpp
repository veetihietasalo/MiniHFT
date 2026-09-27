// Replays an ITCH 5.0 file (by default market_data.itch, written by gen_itch) into order books
// and prints each message and the resulting GOOGL book.
//
// Usage: itch_test [file]

#include <cstdio>
#include <memory>

#include "ItchBook.hpp"
#include "ItchMessages.hpp"
#include "ItchParser.hpp"

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "market_data.itch";
    std::FILE* file = std::fopen(path, "rb");
    if (!file) {
        std::fprintf(stderr, "Failed to open %s (run gen_itch first)\n", path);
        return 1;
    }

    ItchReader reader(file);
    auto builder = std::make_unique<ItchBookBuilder>(1024);
    size_t length = 0;
    while (const uint8_t* m = reader.next(length)) {
        const itch::Header h = itch::decodeHeader(m);
        std::printf("%c  locate %u  t=%llu ns  (%zu bytes)\n", h.type, h.locate,
                    static_cast<unsigned long long>(h.timestamp), length);
        itch::dispatch(m, *builder);
    }
    std::fclose(file);

    const int locate = builder->locateOf("GOOGL");
    if (locate < 0) {
        std::fprintf(stderr, "GOOGL not in the stock directory\n");
        return 1;
    }
    const L3OrderBook& book = builder->book(static_cast<uint16_t>(locate));
    std::printf("\nGOOGL book (best price nearest the middle):\n");
    const auto& asks = book.levels(BookSide::Ask);
    for (size_t i = 0; i < asks.size(); ++i) {
        std::printf("  ASK %9.4f  %6llu shares  %u orders\n", asks[i].price / 1e4,
                    static_cast<unsigned long long>(asks[i].shares), asks[i].orderCount);
    }
    std::printf("  -----------------------------------\n");
    const auto& bids = book.levels(BookSide::Bid);
    for (size_t i = bids.size(); i-- > 0;) {
        std::printf("  BID %9.4f  %6llu shares  %u orders\n", bids[i].price / 1e4,
                    static_cast<unsigned long long>(bids[i].shares), bids[i].orderCount);
    }
    std::printf("  last trade %.4f\n", builder->lastPrice(static_cast<uint16_t>(locate)) / 1e4);

    const ItchBookBuilder::Stats& s = builder->stats();
    return (reader.lengthMismatches() == 0 && s.unknownRefs == 0) ? 0 : 1;
}
