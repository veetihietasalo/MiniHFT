#pragma once

// Shared helpers for the latency benchmarks: machine description, argument parsing
// and the percentile table.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

#include "LatencyHistogram.hpp"

namespace bench {

    inline std::string cpuBrand() {
        unsigned int regs[12] = {};
#if defined(_MSC_VER)
        for (int i = 0; i < 3; ++i) __cpuid(reinterpret_cast<int*>(regs + 4 * i), static_cast<int>(0x80000002u + i));
#else
        for (unsigned int i = 0; i < 3; ++i) __get_cpuid(0x80000002u + i, &regs[4 * i], &regs[4 * i + 1], &regs[4 * i + 2], &regs[4 * i + 3]);
#endif
        char text[49] = {};
        std::memcpy(text, regs, 48);
        std::string brand(text);
        brand.erase(0, brand.find_first_not_of(' '));
        brand.erase(brand.find_last_not_of(' ') + 1);
        return brand;
    }

    inline std::string compilerName() {
#if defined(__clang__)
        return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
        return std::string("GCC ") + __VERSION__;
#elif defined(_MSC_VER)
        return "MSVC " + std::to_string(_MSC_FULL_VER);
#else
        return "unknown compiler";
#endif
    }

    inline const char* osName() {
#if defined(_WIN32)
        return "Windows";
#elif defined(__linux__)
        return "Linux";
#else
        return "unknown OS";
#endif
    }

    inline const char* buildType() {
#if defined(NDEBUG)
        return "optimized (NDEBUG)";
#else
        return "debug";
#endif
    }

    inline void printMachine(double ticksPerNs, uint64_t overheadTicks) {
        std::printf("  cpu       %s (%u logical CPUs visible)\n", cpuBrand().c_str(), std::thread::hardware_concurrency());
        std::printf("  platform  %s, %s, %s build\n", osName(), compilerName().c_str(), buildType());
        std::printf("  tsc       %.3f ticks/ns, timer overhead %.1f ns (subtract from small values)\n",
                    ticksPerNs, static_cast<double>(overheadTicks) / ticksPerNs);
    }

    // --name=value, or `fallback` when absent.
    inline long long argInt(int argc, char** argv, const char* name, long long fallback) {
        const std::string prefix = std::string("--") + name + "=";
        for (int i = 1; i < argc; ++i) {
            if (std::strncmp(argv[i], prefix.c_str(), prefix.size()) == 0) return std::atoll(argv[i] + prefix.size());
        }
        return fallback;
    }

    // --name=a,b,c as integers, or `fallback` when absent.
    inline std::vector<long long> argIntList(int argc, char** argv, const char* name, std::vector<long long> fallback) {
        const std::string prefix = std::string("--") + name + "=";
        for (int i = 1; i < argc; ++i) {
            if (std::strncmp(argv[i], prefix.c_str(), prefix.size()) != 0) continue;
            std::vector<long long> values;
            const char* p = argv[i] + prefix.size();
            while (*p != '\0') {
                char* end = nullptr;
                const long long value = std::strtoll(p, &end, 10);
                if (end == p) break; // not a number: stop parsing
                values.push_back(value);
                p = (*end == ',') ? end + 1 : end;
            }
            return values;
        }
        return fallback;
    }

    inline bool hasFlag(int argc, char** argv, const char* flag) {
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], flag) == 0) return true;
        }
        return false;
    }

    inline void printTableHeader(const char* title) {
        std::printf("\n%-26s %9s %9s %9s %9s %9s %9s %9s\n", title, "min", "p50", "p90", "p99", "p99.9", "p99.99", "max");
    }

    inline void printTableRow(const char* label, const LatencyHistogram& h, double ticksPerNs) {
        auto ns = [&](uint64_t ticks) { return static_cast<double>(ticks) / ticksPerNs; };
        std::printf("%-26s %9.1f %9.1f %9.1f %9.1f %9.1f %9.1f %9.1f\n", label,
                    ns(h.min()), ns(h.valueAtPercentile(50)), ns(h.valueAtPercentile(90)),
                    ns(h.valueAtPercentile(99)), ns(h.valueAtPercentile(99.9)),
                    ns(h.valueAtPercentile(99.99)), ns(h.max()));
    }
}
