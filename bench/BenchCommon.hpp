#pragma once

// Shared helpers for the latency benchmarks: machine description, argument parsing,
// the percentile table, and the --json=FILE results that bench/compare.py reads.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
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

    // --name=value as text, or an empty string when absent.
    inline std::string argString(int argc, char** argv, const char* name) {
        const std::string prefix = std::string("--") + name + "=";
        for (int i = 1; i < argc; ++i) {
            if (std::strncmp(argv[i], prefix.c_str(), prefix.size()) == 0) return argv[i] + prefix.size();
        }
        return {};
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

    // Build type for the JSON metadata. Unlike buildType() it also tells apart a GCC or Clang
    // build without optimization: NDEBUG at -O0 isn't a release build.
    inline const char* jsonBuildType() {
#if !defined(NDEBUG)
        return "debug";
#elif defined(__GNUC__) && !defined(__OPTIMIZE__)
        return "NDEBUG, not optimized";
#else
        return "release";
#endif
    }

    // `text` as a JSON string, quotes included. Control characters are escaped; bytes from 0x80
    // up pass through, so UTF-8 stays UTF-8.
    inline std::string jsonString(std::string_view text) {
        constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(text.size() + 2);
        out += '"';
        for (const char c : text) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: {
                    const auto code = static_cast<unsigned char>(c);
                    if (code < 0x20) {
                        out += "\\u00";
                        out += kHex[code >> 4];
                        out += kHex[code & 0xF];
                    } else {
                        out += c;
                    }
                    break;
                }
            }
        }
        out += '"';
        return out;
    }

    // A finite double as a JSON number. NaN and the infinities have no JSON form: null.
    inline std::string jsonNumber(double value) {
        if (!std::isfinite(value)) return "null";
        char text[32];
        std::snprintf(text, sizeof text, "%.10g", value);
        return text;
    }

    // Machine-readable results, written when the program is given --json=FILE: the run's
    // metadata (benchmark, arguments, CPU, compiler, build type, timer, pinning) and a flat list of
    // metrics with stable names such as orderbook/new/depth=100/take/p99. bench/compare.py compares
    // such files against a baseline. The human-readable output doesn't change.
    //
    // --commit=SHA and --label=NAME are recorded as given. A label keeps apart runs that report the
    // same metric names under a different setup, such as two core pairs in the CCD matrix.
    class JsonReport {
    public:
        enum class Better : std::uint8_t { Lower, Higher };

        JsonReport(const char* benchmark, int argc, char** argv)
            : benchmark_(benchmark), path_(argString(argc, argv, "json")), commit_(argString(argc, argv, "commit")),
              label_(argString(argc, argv, "label")) {
            args_.reserve(static_cast<std::size_t>(argc));
            for (int i = 1; i < argc; ++i) {
                // Where the results go and how they are tagged doesn't change them.
                const std::string_view arg = argv[i];
                if (arg.starts_with("--json=") || arg.starts_with("--commit=") || arg.starts_with("--label=")) continue;
                args_.emplace_back(arg);
            }
        }

        void setTimer(double ticksPerNs, std::uint64_t overheadTicks) {
            ticksPerNs_ = ticksPerNs;
            overheadNs_ = static_cast<double>(overheadTicks) / ticksPerNs;
        }

        // True if every benchmark thread was pinned to its core.
        void setPinned(bool pinned) { pinned_ = pinned; }

        void add(std::string_view name, const char* unit, double value, Better better = Better::Lower) {
            metrics_.push_back({std::string(name), unit, value, better});
        }

        // A histogram of TSC ticks, in ns: <prefix>/min, /p50, /p90, /p99, /p99.9, /p99.99 and /max,
        // the columns of printTableRow. Nothing for an empty histogram.
        void addHistogram(std::string_view prefix, const LatencyHistogram& h, double ticksPerNs) {
            if (h.count() == 0) return;
            const std::string p(prefix);
            const auto ns = [ticksPerNs](std::uint64_t ticks) { return static_cast<double>(ticks) / ticksPerNs; };
            add(p + "/min", "ns", ns(h.min()));
            add(p + "/p50", "ns", ns(h.valueAtPercentile(50)));
            add(p + "/p90", "ns", ns(h.valueAtPercentile(90)));
            add(p + "/p99", "ns", ns(h.valueAtPercentile(99)));
            add(p + "/p99.9", "ns", ns(h.valueAtPercentile(99.9)));
            add(p + "/p99.99", "ns", ns(h.valueAtPercentile(99.99)));
            add(p + "/max", "ns", ns(h.max()));
        }

        // Writes the file if --json was given. False, after a message on stderr, if it can't.
        [[nodiscard]] bool write() const {
            if (path_.empty()) return true;
            std::string out;
            out.reserve(2048 + metrics_.size() * 96);
            out += "{\n  \"schema\": \"minihft-bench/1\",\n  \"metadata\": {\n";
            out += "    \"benchmark\": " + jsonString(benchmark_) + ",\n";
            out += "    \"label\": " + jsonString(label_) + ",\n";
            out += "    \"args\": [";
            for (std::size_t i = 0; i < args_.size(); ++i) {
                if (i > 0) out += ", ";
                out += jsonString(args_[i]);
            }
            out += "],\n";
            out += "    \"commit\": " + jsonString(commit_) + ",\n";
            out += "    \"cpu\": " + jsonString(cpuBrand()) + ",\n";
            out += "    \"logical_cpus\": " + std::to_string(std::thread::hardware_concurrency()) + ",\n";
            out += "    \"os\": " + jsonString(osName()) + ",\n";
            out += "    \"compiler\": " + jsonString(compilerName()) + ",\n";
            out += "    \"build_type\": " + jsonString(jsonBuildType()) + ",\n";
            out += "    \"tsc_ticks_per_ns\": " + jsonNumber(ticksPerNs_) + ",\n";
            out += "    \"timer_overhead_ns\": " + jsonNumber(overheadNs_) + ",\n";
            out += std::string("    \"pinned\": ") + (pinned_ ? "true" : "false") + ",\n";
            out += "    \"unix_time\": " + std::to_string(static_cast<long long>(std::time(nullptr))) + "\n";
            out += "  },\n  \"metrics\": [\n";
            for (std::size_t i = 0; i < metrics_.size(); ++i) {
                const Metric& m = metrics_[i];
                out += "    {\"name\": ";
                out += jsonString(m.name);
                out += ", \"unit\": ";
                out += jsonString(m.unit);
                out += ", \"value\": ";
                out += jsonNumber(m.value);
                out += m.better == Better::Lower ? ", \"better\": \"lower\"}" : ", \"better\": \"higher\"}";
                out += i + 1 < metrics_.size() ? ",\n" : "\n";
            }
            out += "  ]\n}\n";

            std::FILE* file = std::fopen(path_.c_str(), "wb");
            if (file == nullptr) {
                std::fprintf(stderr, "cannot open %s for writing\n", path_.c_str());
                return false;
            }
            const bool written = std::fwrite(out.data(), 1, out.size(), file) == out.size();
            if (std::fclose(file) != 0 || !written) {
                std::fprintf(stderr, "cannot write %s\n", path_.c_str());
                return false;
            }
            return true;
        }

    private:
        struct Metric {
            std::string name;
            std::string unit;
            double value;
            Better better;
        };

        std::string benchmark_;
        std::string path_;
        std::string commit_;
        std::string label_;
        std::vector<std::string> args_;
        std::vector<Metric> metrics_;
        double ticksPerNs_ = 0.0;
        double overheadNs_ = 0.0;
        bool pinned_ = false;
    };
}
