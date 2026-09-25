#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

// Timestamps from the CPU's time-stamp counter. On CPUs with an invariant TSC
// (every Zen and every Intel core since Nehalem) it ticks at a constant rate on
// all cores, so readings from different threads can be subtracted.
namespace Tsc {

    // Plain RDTSC: cheapest, but the CPU may execute it before earlier instructions finish.
    // Use it to stamp the start of an interval.
    inline uint64_t read() noexcept {
        return __rdtsc();
    }

    // RDTSCP waits until every earlier instruction, loads included, has executed.
    // Use it to stamp the end of an interval, so the work being timed is really done.
    inline uint64_t readOrdered() noexcept {
        unsigned int aux;
        return __rdtscp(&aux);
    }

    // Spin-loop hint: frees pipeline resources while polling.
    inline void cpuRelax() noexcept {
        _mm_pause();
    }

    // TSC ticks per nanosecond, measured against steady_clock (median of 3 windows).
    // Busy-waits for about 3 * windowMs.
    inline double calibrateTicksPerNs(int windowMs = 50) {
        std::array<double, 3> samples{};
        for (double& s : samples) {
            const auto t0 = std::chrono::steady_clock::now();
            const uint64_t c0 = readOrdered();
            while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(windowMs)) {}
            const uint64_t c1 = readOrdered();
            const auto t1 = std::chrono::steady_clock::now();
            s = static_cast<double>(c1 - c0) / std::chrono::duration<double, std::nano>(t1 - t0).count();
        }
        std::sort(samples.begin(), samples.end());
        return samples[1];
    }

    // Smallest gap between a read() and the readOrdered() right after it: the floor
    // under every interval measured with that pair.
    inline uint64_t measureOverheadTicks(int iterations = 100'000) {
        uint64_t best = std::numeric_limits<uint64_t>::max();
        for (int i = 0; i < iterations; ++i) {
            const uint64_t a = read();
            const uint64_t b = readOrdered();
            best = std::min(best, b - a);
        }
        return best;
    }
}
