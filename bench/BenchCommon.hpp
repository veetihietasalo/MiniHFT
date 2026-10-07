#pragma once

// Shared helpers for the latency benchmarks: machine description, argument parsing
// and the percentile table.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define MINIHFT_BENCH_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#else
#include <fstream>
#endif

#include "LatencyHistogram.hpp"
#include "Tsc.hpp"

namespace bench {

#if defined(MINIHFT_BENCH_X86)

    inline std::string cpuBrand() {
        unsigned int regs[12] = {};
        // CPUID leaves 0x80000002..4 return the brand string, 16 bytes each.
        for (unsigned int leaf = 0; leaf < 3; ++leaf) {
            unsigned int* r = regs + size_t{4} * leaf;
#if defined(_MSC_VER)
            __cpuid(reinterpret_cast<int*>(r), static_cast<int>(0x80000002u + leaf));
#else
            __get_cpuid(0x80000002u + leaf, &r[0], &r[1], &r[2], &r[3]);
#endif
        }
        char text[49] = {};
        std::memcpy(text, regs, 48);
        std::string brand(text);
        brand.erase(0, brand.find_first_not_of(' '));
        brand.erase(brand.find_last_not_of(' ') + 1);
        return brand;
    }

#else

    // No CPUID: /proc/cpuinfo names the core by implementer and part number (MIDR_EL1) on
    // AArch64 Linux, with a "model name" line only on some kernels. Elsewhere, "unknown".
    inline std::string cpuBrand() {
        std::ifstream cpuinfo("/proc/cpuinfo");
        std::string line;
        std::string implementer;
        std::string part;
        auto value = [&] {
            const size_t start = line.find_first_not_of(" \t", line.find(':') + 1);
            return start == std::string::npos ? std::string() : line.substr(start);
        };
        while (std::getline(cpuinfo, line) && line.find(':') != std::string::npos) { // first CPU only
            if (line.rfind("model name", 0) == 0) return value();
            if (line.rfind("CPU implementer", 0) == 0) implementer = value();
            if (line.rfind("CPU part", 0) == 0) part = value();
        }
        if (implementer.empty() || part.empty()) return "unknown";
        if (implementer == "0x41") { // Arm Ltd: the cores in Graviton, Cobalt, Axion, Grace
            struct Core { const char* part; const char* name; };
            for (const Core c : {Core{"0xd0c", "Neoverse N1"}, Core{"0xd40", "Neoverse V1"}, Core{"0xd49", "Neoverse N2"},
                                 Core{"0xd4f", "Neoverse V2"}, Core{"0xd84", "Neoverse V3"}, Core{"0xd8e", "Neoverse N3"}}) {
                if (part == c.part) return std::string("Arm ") + c.name;
            }
        }
        return "implementer " + implementer + " part " + part;
    }

#endif

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
        std::printf("  %-9s %.3f ticks/ns, timer overhead %.1f ns (subtract from small values)\n",
                    Tsc::kCounterName, ticksPerNs, static_cast<double>(overheadTicks) / ticksPerNs);
        if (ticksPerNs < 1.0) { // e.g. a 25 MHz CNTVCT_EL0: every latency below is a multiple of this
            std::printf("  counter   one tick is %.1f ns, so latencies are quantized to that\n", 1.0 / ticksPerNs);
        }
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
