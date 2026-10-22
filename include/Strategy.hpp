#pragma once

#include "Order.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

// Avellaneda & Stoikov (2008), "High-frequency trading in a limit order book", Quantitative
// Finance 8(3). The mid price moves as dS = sigma dW, and a quote at distance delta from the mid
// is hit at rate A exp(-k delta). For a market maker with exponential utility (risk aversion
// gamma) whose inventory must be flat at time T, the paper's approximate solution is:
//   reservation price  r = s - q gamma sigma^2 (T - t)
//   total spread       delta_ask + delta_bid = gamma sigma^2 (T - t) + (2 / gamma) ln(1 + gamma / k)
// quoted symmetrically around r. q counts units of the quoted size: one fill moves it by one.
class MarketMakerStrategy {
private:
    double riskAversion; // gamma
    double volatility;   // sigma, in price units per sqrt(time unit)
    double fillDecay;    // k, per price unit: how fast fills thin out away from the mid
    double terminalTime; // T, in the same time unit as t
    int inventory;

    double timeLeft(double t) const { return std::max(0.0, terminalTime - t); }

public:
    MarketMakerStrategy(double gamma, double sigma, double k, double horizon)
        : riskAversion(gamma), volatility(sigma), fillDecay(k), terminalTime(horizon), inventory(0) {
        // Negated comparisons also reject NaN.
        if (!(gamma > 0.0) || !(sigma >= 0.0) || !(k > 0.0) || !(horizon >= 0.0)) {
            throw std::invalid_argument("MarketMakerStrategy needs gamma > 0, sigma >= 0, k > 0 and horizon >= 0");
        }
    }

    void onTrade(const Trade& trade, Side mySide) {
        if (mySide == Side::Buy) {
            inventory += static_cast<int>(trade.quantity);
        } else {
            inventory -= static_cast<int>(trade.quantity);
        }
    }

    // r = s - q gamma sigma^2 (T - t): a long position lowers both quotes, a short one raises them.
    // After T the inventory term is zero.
    double getReservationPrice(double midPrice, double t) const {
        return midPrice - static_cast<double>(inventory) * riskAversion * volatility * volatility * timeLeft(t);
    }

    // gamma sigma^2 (T - t) + (2 / gamma) ln(1 + gamma / k). The second term doesn't depend on time
    // or inventory, and tends to 2 / k as gamma goes to 0.
    double getSpread(double t) const {
        return riskAversion * volatility * volatility * timeLeft(t) +
               (2.0 / riskAversion) * std::log1p(riskAversion / fillDecay);
    }

    // Bid and ask on a one-cent grid. The bid rounds down and the ask up, so the quotes are never
    // tighter than the model's.
    std::pair<double, double> getQuotes(double midPrice, double t) const {
        const double reservation = getReservationPrice(midPrice, t);
        const double half = getSpread(t) / 2.0;
        constexpr double onGrid = 1e-9; // a price already on the grid, e.g. 99.15, stays there
        return { std::floor((reservation - half) * 100.0 + onGrid) / 100.0,
                 std::ceil((reservation + half) * 100.0 - onGrid) / 100.0 };
    }

    int getInventory() const { return inventory; }
};
