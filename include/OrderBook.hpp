#pragma once

#include <vector>
#include <algorithm>
#include <iostream>
#include "Order.hpp"
#include "ObjectPool.hpp"

class OrderBook {
private:
    // Flat storage for cache locality
    // Bids: Sorted High to Low
    std::vector<Order> bids;
    // Asks: Sorted Low to High
    std::vector<Order> asks;

public:
    void addOrder(const Order& order) {
        if (order.side == Side::Buy) {
            // Insert keeping sorted order (High to Low)
            auto it = std::lower_bound(bids.begin(), bids.end(), order,
                [](const Order& a, const Order& b) {
                    return a.price > b.price; // Descending
                });
            bids.insert(it, order);
        } else {
            // Insert keeping sorted order (Low to High)
            auto it = std::lower_bound(asks.begin(), asks.end(), order,
                [](const Order& a, const Order& b) {
                    return a.price < b.price; // Ascending
                });
            asks.insert(it, order);
        }
    }

    std::vector<Trade> match() {
        std::vector<Trade> trades;

        while (!bids.empty() && !asks.empty()) {
            Order& bestBid = bids.front();
            Order& bestAsk = asks.front();

            if (bestBid.price >= bestAsk.price) {
                Quantity tradeQty = std::min(bestBid.quantity, bestAsk.quantity);

                trades.push_back({
                    bestAsk.price, // Trade at ask price (maker)
                    tradeQty,
                    bestBid.id,
                    bestAsk.id,
                    (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count()
                });

                bestBid.quantity -= tradeQty;
                bestAsk.quantity -= tradeQty;

                if (bestBid.quantity == 0) bids.erase(bids.begin());
                if (bestAsk.quantity == 0) asks.erase(asks.begin());
            } else {
                break;
            }
        }
        return trades;
    }

    void printBook() const {
        std::cout << "\n--- Order Book (Optimized) ---\n";
        std::cout << "ASKS:\n";
        // Print top 5 asks (reverse iterator to show highest of the low first? No, standard order)
        // Asks are Low -> High. We want to print High -> Low visually if we were doing a ladder,
        // but for simple list:
        for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
             std::cout << it->price << " : " << it->quantity << "\n";
        }
        std::cout << "------\n";
        std::cout << "BIDS:\n";
        for (const auto& o : bids) {
            std::cout << o.price << " : " << o.quantity << "\n";
        }
        std::cout << "------------------\n";
    }
};
