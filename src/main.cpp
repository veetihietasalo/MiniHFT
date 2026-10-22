#include <cmath>
#include <cstdio>
#include <exception>
#include <random>
#include <vector>

#include "Order.hpp"
#include "Statistics.hpp"
#include "Strategy.hpp"

// Avellaneda & Stoikov's own simulation, run with MarketMakerStrategy. Each step of dt the mid
// moves up or down by sigma sqrt(dt) with equal probability, and each quote is hit with
// probability A exp(-k delta) dt, where delta is its distance from the mid. A fill is one unit.
// The paper compares its inventory strategy with a symmetric one, which quotes the inventory
// strategy's average spread, held constant, around the mid instead of around the reservation
// price; so does this program.
namespace {

constexpr double kGamma = 0.1, kSigma = 2.0, kK = 1.5, kA = 140.0, kT = 1.0, kS0 = 100.0;
constexpr int kSteps = 200; // dt = T / 200 = 0.005
constexpr double kDt = kT / kSteps;
constexpr int kRuns = 1000;

struct Run {
    double pnl;        // cash plus the final inventory at the final mid
    double inventory;  // final inventory
    double meanSpread; // the quoted spread averaged over the run
};

// The inventory strategy's spread averaged over the steps: the symmetric strategy's constant spread.
double averageSpread() {
    const MarketMakerStrategy mm(kGamma, kSigma, kK, kT);
    double sum = 0.0;
    for (int i = 0; i < kSteps; ++i) sum += mm.getSpread(i * kDt);
    return sum / kSteps;
}

// `print` shows every 25th step of this run.
Run simulate(std::mt19937_64& rng, bool aroundReservation, bool print) {
    static const double symmetricSpread = averageSpread();
    MarketMakerStrategy mm(kGamma, kSigma, kK, kT);
    std::bernoulli_distribution up(0.5);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    const double move = kSigma * std::sqrt(kDt);

    double s = kS0, cash = 0.0, spreadSum = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        const double t = i * kDt;
        const double spread = aroundReservation ? mm.getSpread(t) : symmetricSpread;
        const double centre = aroundReservation ? mm.getReservationPrice(s, t) : s;
        const double bid = centre - spread / 2.0, ask = centre + spread / 2.0;
        spreadSum += spread;
        if (print && i % 25 == 0) {
            std::printf("  t %.3f  mid %7.2f  inventory %+3lld  quotes %7.2f @ %7.2f\n", t, s,
                        static_cast<long long>(mm.getInventory()), bid, ask);
        }

        if (uniform(rng) < kA * std::exp(-kK * (s - bid)) * kDt) {
            cash -= bid;
            mm.onTrade(Trade{bid, 1, 0, 0, 0}, Side::Buy);
        }
        if (uniform(rng) < kA * std::exp(-kK * (ask - s)) * kDt) {
            cash += ask;
            mm.onTrade(Trade{ask, 1, 0, 0, 0}, Side::Sell);
        }
        s += up(rng) ? move : -move;
    }
    const auto q = static_cast<double>(mm.getInventory());
    return { cash + q * s, q, spreadSum / kSteps };
}

void summarize(const char* name, const std::vector<Run>& runs) {
    std::vector<double> pnl, inventory, spread;
    for (const Run& r : runs) {
        pnl.push_back(r.pnl);
        inventory.push_back(r.inventory);
        spread.push_back(r.meanSpread);
    }
    std::printf("%-20s %8.2f %10.2f %12.2f %12.2f %14.2f\n", name, Statistics::mean(spread), Statistics::mean(pnl),
                Statistics::stdDev(pnl), Statistics::mean(inventory), Statistics::stdDev(inventory));
}

int run() {
    std::printf("Avellaneda-Stoikov simulation: gamma %.1f, sigma %.0f, k %.1f, A %.0f, T %.0f, dt %.3f, s0 %.0f\n\n",
                kGamma, kSigma, kK, kA, kT, kDt, kS0);
    std::mt19937_64 rng(42); // fixed seed: the same output every run

    std::printf("One run of the inventory strategy:\n");
    simulate(rng, true, true);

    std::vector<Run> inventoryRuns, symmetricRuns;
    for (int i = 0; i < kRuns; ++i) {
        inventoryRuns.push_back(simulate(rng, true, false));
        symmetricRuns.push_back(simulate(rng, false, false));
    }
    std::printf("\n%d runs each:\n%-20s %8s %10s %12s %12s %14s\n", kRuns, "Strategy", "Spread", "Profit", "Std(profit)",
                "Final q", "Std(final q)");
    summarize("Inventory", inventoryRuns);
    summarize("Symmetric", symmetricRuns);
    return 0;
}

} // namespace

int main() {
    try {
        return run();
    } catch (const std::exception& e) { // MarketMakerStrategy rejects parameters the model can't use
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
