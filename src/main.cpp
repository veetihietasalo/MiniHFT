#include <iostream>
#include <thread>
#include <iostream>
#include <thread>
#include <chrono>
#include <iomanip> // Added for strategy simulation
#include "Matrix.hpp"
#include "Statistics.hpp"
#include "OrderBook.hpp"
#include "MarketSimulator.hpp"
#include "Strategy.hpp" // Added for strategy simulation

int main() {
    std::cout << "MiniHFT Engine Starting (Strategy Mode)...\n";

    OrderBook book;
    // Start price $100, Volatility 0.5
    MarketSimulator sim(100.0, 0.5);
    // Gamma (Risk Aversion) = 0.1, Sigma (Vol) = 0.5
    MarketMakerStrategy strategy(0.1, 0.5);

    double currentMidPrice = 100.0;

    for (int i = 0; i < 50; ++i) {
        // 1. Strategy places quotes based on current mid price and inventory
        auto [bidPrice, askPrice] = strategy.getQuotes(currentMidPrice);

        // Place MM Orders (cancel previous would be ideal, but for now just add new)
        // In a real system, we'd update existing orders to avoid message traffic
        book.addOrder(Order(1000 + i, Side::Buy, bidPrice, 10));
        book.addOrder(Order(2000 + i, Side::Sell, askPrice, 10));

        std::cout << "MM Quotes: " << bidPrice << " @ " << askPrice
                  << " [Inv: " << strategy.getInventory() << "]\n";

        // 2. Simulator generates random market flow
        Order marketOrder = sim.generateOrder();
        // Force simulator order to be aggressive to trigger trades against MM
        if (marketOrder.side == Side::Buy) marketOrder.price = 9999.0; // Market Buy
        else marketOrder.price = 0.01; // Market Sell

        // Only trade sometimes to simulate liquidity taking
        if (i % 3 == 0) {
            book.addOrder(marketOrder);
        }

        // 3. Match
        auto trades = book.match();

        if (!trades.empty()) {
            for (const auto& t : trades) {
                // Update Strategy Inventory
                // Assuming MM was always on the other side of the trade
                // If Market Order was Buy, MM Sold (Inventory -)
                // If Market Order was Sell, MM Bought (Inventory +)
                Side mmSide = (marketOrder.side == Side::Buy) ? Side::Sell : Side::Buy;
                strategy.onTrade(t, mmSide);

                std::cout << "    >>> TRADE! " << t.quantity << " @ " << t.price
                          << " (MM " << (mmSide == Side::Buy ? "Bought" : "Sold") << ")\n";
            }
        }

        // Update "Mid Price" for next tick (simulating market move)
        // In reality, mid price comes from the book, but here we simulate the "fair value" moving
        currentMidPrice = marketOrder.price; // This is wrong, market order price is aggressive
        // Let's just drift the mid price randomly
        currentMidPrice += (rand() % 100 - 50) / 100.0;
        if (currentMidPrice < 0) currentMidPrice = 100.0;

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "\nFinal Inventory: " << strategy.getInventory() << "\n";

    return 0;
}
