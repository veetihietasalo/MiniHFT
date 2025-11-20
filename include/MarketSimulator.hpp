#pragma once

#include <random>
#include "Order.hpp"

class MarketSimulator {
private:
    std::mt19937 gen;
    std::normal_distribution<> priceDist;
    std::uniform_int_distribution<> qtyDist;
    std::uniform_int_distribution<> sideDist;
    double currentPrice;
    OrderId nextId = 1;

public:
    MarketSimulator(double startPrice, double volatility)
        : gen(std::random_device{}()),
          priceDist(0.0, volatility),
          qtyDist(1, 100),
          sideDist(0, 1),
          currentPrice(startPrice) {}

    Order generateOrder() {
        // Random walk
        currentPrice += priceDist(gen);
        if (currentPrice < 0.01) currentPrice = 0.01;

        Side side = (sideDist(gen) == 0) ? Side::Buy : Side::Sell;

        // If Buy, price slightly lower; if Sell, slightly higher (to create spread)
        double orderPrice = currentPrice + ((side == Side::Buy) ? -0.05 : 0.05);

        // Round to 2 decimal places
        orderPrice = std::round(orderPrice * 100.0) / 100.0;

        return Order(nextId++, side, orderPrice, qtyDist(gen));
    }
};
