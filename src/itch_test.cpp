#include <iostream>
#include "ItchParser.hpp"

int main() {
    ItchParser parser;
    if (!parser.loadFile("market_data.itch")) {
        std::cerr << "Failed to load market_data.itch\n";
        return 1;
    }

    std::cout << "Parsing ITCH File...\n";

    int count = 0;
    while (const ItchHeader* header = parser.next()) {
        if (header->messageType == 'A') {
            const AddOrderMsg* add = ItchParser::asAddOrder(header);

            // Swap back to host endianness to print
            uint64_t ref = swap64(add->orderReferenceNumber);
            uint32_t price = swap32(add->price);
            uint32_t shares = swap32(add->shares);
            std::string stock(add->stock, 8);

            std::cout << "Msg " << count++ << ": [ADD] "
                      << (add->buySellIndicator == 'B' ? "BUY " : "SELL ")
                      << shares << " " << stock << " @ "
                      << (price / 10000.0) << "\n";
        }
    }

    return 0;
}
