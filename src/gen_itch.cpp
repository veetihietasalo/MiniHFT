#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include "ItchMessages.hpp"

int main() {
    std::ofstream file("market_data.itch", std::ios::binary);
    if (!file) {
        std::cerr << "Failed to create file.\n";
        return 1;
    }

    std::cout << "Generating ITCH data...\n";

    for (int i = 0; i < 10; ++i) {
        AddOrderMsg msg;
        // Header
        msg.header.messageType = 'A';
        msg.header.stockLocate = swap32(1); // Dummy
        msg.header.trackingNumber = swap32(i);
        msg.header.timestamp = swap64(1000000 + i * 100);

        // Body
        msg.orderReferenceNumber = swap64(1000 + i);
        msg.buySellIndicator = (i % 2 == 0) ? 'B' : 'S';
        msg.shares = swap32(100);
        std::strncpy(msg.stock, "GOOGL   ", 8);
        msg.price = swap32(1500000 + i * 100); // 150.0000

        file.write(reinterpret_cast<char*>(&msg), sizeof(AddOrderMsg));
    }

    file.close();
    std::cout << "Done. Created market_data.itch\n";
    return 0;
}
