#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
// AArch64 with GCC or Clang: the generic timer's virtual counter, read with inline asm.
#else
#error "Tsc.hpp supports the x86 time-stamp counter and, with GCC/Clang, the AArch64 generic timer"
#endif

// Timestamps from a constant-rate counter that every core shares, so readings from
// different threads can be subtracted:
//   - x86: the time-stamp counter. Invariant on every Zen and every Intel core since Nehalem.
//   - AArch64: the generic timer's virtual count, CNTVCT_EL0. The architecture requires it to
//     be monotonic and system-wide. It ticks far slower than a TSC: often 24-200 MHz, and
//     1 GHz from Armv8.6. Calibrate rather than assume a rate.
namespace Tsc {

#if defined(__aarch64__)

    inline constexpr const char* kCounterName = "cntvct";

    // Plain MRS: cheapest, but the CPU may read the counter before earlier instructions finish.
    // Use it to stamp the start of an interval.
    [[nodiscard]] inline uint64_t read() noexcept {
        uint64_t ticks;
        asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
        return ticks;
    }

    // ISB first: the counter is read only after every earlier instruction has completed,
    // as with RDTSCP. Use it to stamp the end of an interval, so the work being timed is
    // really done. The memory clobber keeps the compiler from sinking that work below it.
    [[nodiscard]] inline uint64_t readOrdered() noexcept {
        uint64_t ticks;
        asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(ticks) : : "memory");
        return ticks;
    }

    // Spin-loop hint. ISB rather than YIELD: YIELD is a no-op on Neoverse cores, while ISB
    // stalls the pipeline for a few dozen cycles, like x86 PAUSE.
    inline void cpuRelax() noexcept {
        asm volatile("isb");
    }

    // The counter's nominal rate as firmware programmed it (CNTFRQ_EL0). x86 has no reliable
    // equivalent, so the benchmarks calibrate on both.
    [[nodiscard]] inline uint64_t nominalHz() noexcept {
        uint64_t hz;
        asm volatile("mrs %0, cntfrq_el0" : "=r"(hz));
        return hz;
    }

#else

    inline constexpr const char* kCounterName = "tsc";

    // Plain RDTSC: cheapest, but the CPU may execute it before earlier instructions finish.
    // Use it to stamp the start of an interval.
    [[nodiscard]] inline uint64_t read() noexcept {
        return __rdtsc();
    }

    // RDTSCP waits until every earlier instruction, loads included, has executed.
    // Use it to stamp the end of an interval, so the work being timed is really done.
    [[nodiscard]] inline uint64_t readOrdered() noexcept {
        unsigned int aux;
        return __rdtscp(&aux);
    }

    // Spin-loop hint: frees pipeline resources while polling.
    inline void cpuRelax() noexcept {
        _mm_pause();
    }

#endif

    // Counter ticks per nanosecond, measured against steady_clock (median of 3 windows).
    // Busy-waits for about 3 * windowMs.
    [[nodiscard]] inline double calibrateTicksPerNs(int windowMs = 50) {
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
    [[nodiscard]] inline uint64_t measureOverheadTicks(int iterations = 100'000) {
        uint64_t best = std::numeric_limits<uint64_t>::max();
        for (int i = 0; i < iterations; ++i) {
            const uint64_t a = read();
            const uint64_t b = readOrdered();
            best = std::min(best, b - a);
        }
        return best;
    }
}
