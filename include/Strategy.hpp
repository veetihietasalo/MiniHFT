#pragma once

#include "Order.hpp"
#include <cmath>
#include <algorithm>

class MarketMakerStrategy {
private:
    double riskAversion; // Gamma
    double volatility;   // Sigma
    double timeHorizon;  // T - t (simplified as constant 1.0 for now)
    int inventory;

public:
    MarketMakerStrategy(double gamma, double sigma)
        : riskAversion(gamma), volatility(sigma), timeHorizon(1.0), inventory(0) {}

    void onTrade(const Trade& trade, Side mySide) {
        if (mySide == Side::Buy) {
            inventory += trade.quantity;
        } else {
            inventory -= trade.quantity;
        }
    }

    // Avellaneda-Stoikov Reservation Price
    // r(s, t) = s - q * gamma * sigma^2 * (T - t)
    double getReservationPrice(double midPrice) const {
        return midPrice - (inventory * riskAversion * std::pow(volatility, 2) * timeHorizon);
    }

    // Calculate optimal bid/ask quotes
    std::pair<double, double> getQuotes(double midPrice) const {
        double reservation = getReservationPrice(midPrice);
        double spread = 2.0 * riskAversion * std::pow(volatility, 2) * timeHorizon + std::log(1.0 + riskAversion / 0.5); // Simplified spread term

        // Ensure minimum spread of 0.01
        spread = std::max(spread, 0.02);

        double bid = reservation - (spread / 2.0);
        double ask = reservation + (spread / 2.0);

        return { std::round(bid * 100.0) / 100.0, std::round(ask * 100.0) / 100.0 };
    }

    int getInventory() const { return inventory; }
};
