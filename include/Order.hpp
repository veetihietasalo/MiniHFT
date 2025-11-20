#pragma once

#include <cstdint>
#include <string>
#include <chrono>

enum class Side {
    Buy,
    Sell
};

using OrderId = uint64_t;
using Price = double; // In real HFT, use fixed-point (int64_t)
using Quantity = uint32_t;

struct Order {
    OrderId id;
    Side side;
    Price price;
    Quantity quantity;
    uint64_t timestamp; // Nanoseconds

    Order(OrderId i, Side s, Price p, Quantity q)
        : id(i), side(s), price(p), quantity(q) {
        timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    }
};

struct Trade {
    Price price;
    Quantity quantity;
    OrderId buyOrderId;
    OrderId sellOrderId;
    uint64_t timestamp;
};
