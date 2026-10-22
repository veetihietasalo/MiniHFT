#pragma once

#include "Order.hpp"
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>

// Avellaneda & Stoikov (2008), "High-frequency trading in a limit order book", Quantitative
// Finance 8(3). The mid price moves as dS = sigma dW, and a quote at distance delta from the mid
// is hit at rate A exp(-k delta). The market maker maximizes the expected exponential utility
// (risk aversion gamma) of its wealth at time T, with any inventory left then valued at the mid.
// The paper's approximate solution, its equations (29) and (30):
//   reservation price  r = s - q gamma sigma^2 (T - t)
//   total spread       delta_ask + delta_bid = gamma sigma^2 (T - t) + (2 / gamma) ln(1 + gamma / k)
// quoted symmetrically around r. q is the position in shares, and the formula assumes each fill
// is one share, as in the paper; onTrade adds whatever quantity a fill carries.
class MarketMakerStrategy {
private:
    double riskAversion; // gamma
    double volatility;   // sigma, in price units per sqrt(time unit)
    double fillDecay;    // k, per price unit: how fast fills thin out away from the mid
    double terminalTime; // T, in the same time unit as t
    int64_t inventory;

    // A NaN t gives NaN, so a broken clock shows up in the quotes instead of reading as "past T".
    double timeLeft(double t) const {
        const double left = terminalTime - t;
        return left < 0.0 ? 0.0 : left;
    }

public:
    MarketMakerStrategy(double gamma, double sigma, double k, double horizon)
        : riskAversion(gamma), volatility(sigma), fillDecay(k), terminalTime(horizon), inventory(0) {
        // isnormal rejects 0, subnormals (2 / gamma would overflow), infinities and NaN.
        const bool valid = std::isnormal(gamma) && gamma > 0.0 && std::isnormal(k) && k > 0.0 &&
                           std::isfinite(sigma) && sigma >= 0.0 && std::isfinite(horizon) && horizon >= 0.0;
        if (!valid) {
            throw std::invalid_argument("MarketMakerStrategy needs finite gamma > 0, sigma >= 0, k > 0 and horizon >= 0");
        }
    }

    void onTrade(const Trade& trade, Side mySide) {
        if (mySide == Side::Buy) {
            inventory += static_cast<int64_t>(trade.quantity);
        } else {
            inventory -= static_cast<int64_t>(trade.quantity);
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

    // Cent rounding that keeps a price already on the grid where it is: 0.57 * 100 is
    // 56.999999999999993 in binary, and a plain floor would make it 0.56. Exact up to about $1M.
    static double floorToCent(double price) { return std::floor(price * 100.0 + 1e-9) / 100.0; }
    static double ceilToCent(double price) { return std::ceil(price * 100.0 - 1e-9) / 100.0; }

    // Bid and ask on a one-cent grid. The bid rounds down and the ask up, so the quotes are never
    // tighter than the model's. A large enough position can skew both to zero or below; that's the
    // model's answer, and the caller decides what to do with it.
    std::pair<double, double> getQuotes(double midPrice, double t) const {
        const double reservation = getReservationPrice(midPrice, t);
        const double half = getSpread(t) / 2.0;
        return { floorToCent(reservation - half), ceilToCent(reservation + half) };
    }

    int64_t getInventory() const { return inventory; }
};
