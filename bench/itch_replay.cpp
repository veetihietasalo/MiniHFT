// Replays a NASDAQ ITCH 5.0 file through ItchBookBuilder: every instrument's book, every order
// event. Reports end-to-end throughput, the latency of each book update, consistency checks,
// and top-of-book snapshots for a few symbols at fixed times of day.
//
// Usage: itch_replay <file | -> [--symbols=AAPL,MSFT,AMZN,TSLA,SPY] [--no-latency] [--core=2]
//                    [--listener=none|template|virtual]
//   gzip -dc 12302019.NASDAQ_ITCH50.gz | ./itch_replay -
//
// --listener attaches a top-of-book listener to every book (W05): none, one called through
// the builder's template parameter, or one behind a virtual interface.
//
// Exit code 1 if any consistency check fails.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "BenchCommon.hpp"
#include "ItchBook.hpp"
#include "ItchMessages.hpp"
#include "ItchParser.hpp"
#include "LatencyHistogram.hpp"
#include "Listeners.hpp"
#include "ThreadUtils.hpp"
#include "Tsc.hpp"

namespace {

constexpr uint64_t kNsPerHour = 3'600'000'000'000ULL;
constexpr uint64_t kNsPerMinute = 60'000'000'000ULL;

std::vector<std::string> argSymbols(int argc, char** argv) {
    std::string list = "AAPL,MSFT,AMZN,TSLA,SPY";
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--symbols=", 10) == 0) list = argv[i] + 10;
    }
    std::vector<std::string> symbols;
    for (size_t start = 0; start <= list.size();) {
        const size_t comma = list.find(',', start);
        const size_t end = comma == std::string::npos ? list.size() : comma;
        if (end > start) symbols.push_back(list.substr(start, end - start));
        start = end + 1;
    }
    return symbols;
}

std::string argListener(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--listener=", 11) == 0) return argv[i] + 11;
    }
    return "none";
}

std::string clock(uint64_t ns) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu", static_cast<unsigned long long>(ns / kNsPerHour),
                  static_cast<unsigned long long>(ns / kNsPerMinute % 60),
                  static_cast<unsigned long long>(ns / 1'000'000'000ULL % 60));
    return buf;
}

template <typename Builder>
void printSnapshot(uint64_t atNs, const Builder& builder, const std::vector<std::string>& symbols) {
    for (const std::string& symbol : symbols) {
        const int locate = builder.locateOf(symbol);
        if (locate < 0) {
            std::printf("  %s  %-5s  not in the stock directory\n", clock(atNs).c_str(), symbol.c_str());
            continue;
        }
        const L3OrderBook& book = builder.book(static_cast<uint16_t>(locate));
        const PriceLevel* bid = book.bestBid();
        const PriceLevel* ask = book.bestAsk();
        std::printf("  %s  %-5s  bid %10.4f x %-7llu  ask %10.4f x %-7llu  last %10.4f  levels %zu/%zu\n",
                    clock(atNs).c_str(), symbol.c_str(), bid ? bid->price / 1e4 : 0.0,
                    static_cast<unsigned long long>(bid ? bid->shares : 0), ask ? ask->price / 1e4 : 0.0,
                    static_cast<unsigned long long>(ask ? ask->shares : 0),
                    builder.lastPrice(static_cast<uint16_t>(locate)) / 1e4, book.levels(BookSide::Bid).size(),
                    book.levels(BookSide::Ask).size());
    }
}

// Latency histograms per kind of order message.
struct Latencies {
    LatencyHistogram all, add, execute, cancel, remove, replace;

    LatencyHistogram* forType(char type) {
        switch (type) {
            case 'A': case 'F': return &add;
            case 'E': case 'C': return &execute;
            case 'X': return &cancel;
            case 'D': return &remove;
            case 'U': return &replace;
            default: return nullptr;
        }
    }
};

template <typename Builder>
int replay(Builder& builder, std::FILE* file, bool timeEach, const std::vector<std::string>& symbols, double ticksPerNs) {
    const std::vector<uint64_t> snapshotTimes = {
        9 * kNsPerHour + 30 * kNsPerMinute, 10 * kNsPerHour, 12 * kNsPerHour,
        15 * kNsPerHour + 59 * kNsPerMinute, 16 * kNsPerHour + kNsPerMinute};
    size_t nextSnapshot = 0;

    auto latencies = std::make_unique<Latencies>();
    ItchReader reader(file, size_t{4} << 20);
    uint64_t bookTicks = 0;
    uint64_t bookMessages = 0;

    std::printf("Top of book (price x shares; last = last execution):\n");
    const auto wallStart = std::chrono::steady_clock::now();
    size_t length = 0;
    while (const uint8_t* m = reader.next(length)) {
        if (nextSnapshot < snapshotTimes.size() && itch::be48(m + 5) >= snapshotTimes[nextSnapshot]) {
            printSnapshot(snapshotTimes[nextSnapshot++], builder, symbols);
        }
        LatencyHistogram* h = latencies->forType(char(m[0]));
        if (timeEach && h) {
            const uint64_t t0 = Tsc::read();
            itch::dispatch(m, builder);
            const uint64_t t1 = Tsc::readOrdered();
            h->record(t1 - t0);
            latencies->all.record(t1 - t0);
            bookTicks += t1 - t0;
            ++bookMessages;
        } else {
            itch::dispatch(m, builder);
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
    while (nextSnapshot < snapshotTimes.size()) printSnapshot(snapshotTimes[nextSnapshot++], builder, symbols);

    const ItchBookStats& s = builder.stats();
    auto ull = [](uint64_t v) { return static_cast<unsigned long long>(v); };

    std::printf("\nInput\n");
    std::printf("  %llu messages, %.2f GB, %.1f s: %.2f M messages/s end to end (read + parse + book)\n",
                ull(reader.messages()), reader.bytes() / 1e9, seconds, reader.messages() / seconds / 1e6);
    if (timeEach && bookMessages > 0) {
        std::printf("  book updates alone: %.1f ns per order message on average (%llu order messages)\n",
                    static_cast<double>(bookTicks) / ticksPerNs / static_cast<double>(bookMessages), ull(bookMessages));
    }

    std::printf("\nOrder events\n");
    std::printf("  adds %llu, executions %llu, cancels %llu, deletes %llu, replaces %llu, hidden trades %llu\n",
                ull(s.adds), ull(s.executions), ull(s.cancels), ull(s.deletes), ull(s.replaces), ull(s.trades));
    std::printf("  live orders: peak %llu, at end %llu\n", ull(s.peakLiveOrders), ull(s.liveOrders));

    uint64_t depthTotal = 0;
    for (uint64_t c : s.depthHistogram) depthTotal += c;
    const char* depthLabels[] = {"top", "2nd", "3rd-5th", "6th-10th", "11th-50th", "51st+"};
    std::printf("  where book changes land, in price levels from the top of their side:\n   ");
    for (size_t i = 0; i < s.depthHistogram.size(); ++i) {
        std::printf(" %s %.1f%%", depthLabels[i], depthTotal ? 100.0 * s.depthHistogram[i] / depthTotal : 0.0);
    }
    std::printf("\n");

    std::printf("\nConsistency checks (all should be 0)\n");
    std::printf("  wrong message lengths %llu, unknown message types %llu, truncated input %s\n",
                ull(reader.lengthMismatches()), ull(reader.unknownTypes()), reader.truncated() ? "YES" : "no");
    std::printf("  events for unknown orders %llu, over-executions %llu\n", ull(s.unknownRefs), ull(s.overExecutions));
    std::printf("  crossed books during market hours: %llu while the symbol was trading, %llu while halted, paused or quote-only\n",
                ull(s.crossedWhileTrading), ull(s.crossedDuringMarket - s.crossedWhileTrading));
    std::printf("  (trading-action messages: %llu)\n", ull(s.tradingActions));

    std::vector<uint16_t> crossedSymbols;
    for (size_t loc = 0; loc < Builder::kMaxLocates; ++loc) {
        if (builder.crossedCount(static_cast<uint16_t>(loc)) > 0) crossedSymbols.push_back(static_cast<uint16_t>(loc));
    }
    std::sort(crossedSymbols.begin(), crossedSymbols.end(),
              [&](uint16_t a, uint16_t b) { return builder.crossedCount(a) > builder.crossedCount(b); });
    for (const CrossedEvent& e : builder.crossedWhileTrading()) {
        const std::string sinceChange = e.lastTradingAction == 0
            ? std::string("no trading-state change that day")
            : std::to_string((e.timestamp - e.lastTradingAction) / 1000) + " us after its trading state last changed";
        std::printf("  crossed while trading: %-6s at %s.%06llu, %s\n", builder.symbol(e.locate).c_str(),
                    clock(e.timestamp).c_str(), static_cast<unsigned long long>(e.timestamp / 1000 % 1'000'000),
                    sinceChange.c_str());
    }
    if (!crossedSymbols.empty()) {
        std::printf("  crossed most often (%zu symbols in total):", crossedSymbols.size());
        for (size_t i = 0; i < std::min<size_t>(crossedSymbols.size(), 8); ++i) {
            std::printf(" %s %u (state at close '%c')%s", builder.symbol(crossedSymbols[i]).c_str(),
                        builder.crossedCount(crossedSymbols[i]), builder.tradingState(crossedSymbols[i]),
                        i + 1 < std::min<size_t>(crossedSymbols.size(), 8) ? "," : "");
        }
        std::printf("\n");
    }

    if (timeEach) {
        bench::printTableHeader("book update (ns)");
        bench::printTableRow("all order messages", latencies->all, ticksPerNs);
        bench::printTableRow("add (A, F)", latencies->add, ticksPerNs);
        bench::printTableRow("execute (E, C)", latencies->execute, ticksPerNs);
        bench::printTableRow("cancel (X)", latencies->cancel, ticksPerNs);
        bench::printTableRow("delete (D)", latencies->remove, ticksPerNs);
        bench::printTableRow("replace (U)", latencies->replace, ticksPerNs);
    }

    const bool ok = reader.lengthMismatches() == 0 && reader.unknownTypes() == 0 && !reader.truncated() &&
                    s.unknownRefs == 0 && s.overExecutions == 0;
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-' || std::strcmp(argv[i], "-") == 0) path = argv[i];
    }
    if (!path || bench::hasFlag(argc, argv, "--help")) {
        std::printf("usage: itch_replay <file | -> [--symbols=AAPL,MSFT,AMZN,TSLA,SPY] [--no-latency] [--core=2]\n"
                    "                   [--listener=none|template|virtual]\n"
                    "  gzip -dc 12302019.NASDAQ_ITCH50.gz | ./itch_replay -\n");
        return path ? 0 : 2;
    }
    const bool timeEach = !bench::hasFlag(argc, argv, "--no-latency");
    const int core = static_cast<int>(bench::argInt(argc, argv, "core", 2));
    const std::vector<std::string> symbols = argSymbols(argc, argv);
    const std::string listener = argListener(argc, argv);
    if (listener != "none" && listener != "template" && listener != "virtual") {
        std::fprintf(stderr, "--listener must be none, template or virtual\n");
        return 2;
    }

    std::FILE* file = std::strcmp(path, "-") == 0 ? stdin : std::fopen(path, "rb");
    if (!file) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }

    const bool pinned = ThreadUtils::pinThread(core);
    const double ticksPerNs = Tsc::calibrateTicksPerNs();
    const uint64_t overhead = Tsc::measureOverheadTicks();

    std::printf("MiniHFT itch_replay: every order event into per-instrument L3 books\n");
    bench::printMachine(ticksPerNs, overhead);
    std::printf("  thread    core %d (%s), per-message timing %s, top-of-book listener: %s\n\n", core,
                pinned ? "pinned" : "NOT pinned", timeEach ? "on" : "off", listener.c_str());

    constexpr size_t kExpectedLiveOrders = size_t{1} << 22;
    int status = 0;
    if (listener == "template") {
        auto builder = std::make_unique<BasicItchBookBuilder<QuoteCounter>>(kExpectedLiveOrders);
        status = replay(*builder, file, timeEach, symbols, ticksPerNs);
        std::printf("\nListener (template): %llu top-of-book updates, checksum %llu\n",
                    static_cast<unsigned long long>(builder->listener().updates),
                    static_cast<unsigned long long>(builder->listener().checksum));
    } else if (listener == "virtual") {
        CountingHandler counting;
        IgnoringHandler ignoring;
        TopOfBookHandler* handler = g_useIgnoringHandler ? static_cast<TopOfBookHandler*>(&ignoring) : &counting;
        auto builder = std::make_unique<BasicItchBookBuilder<VirtualListener>>(kExpectedLiveOrders, VirtualListener{handler});
        status = replay(*builder, file, timeEach, symbols, ticksPerNs);
        std::printf("\nListener (virtual): %llu top-of-book updates, checksum %llu\n",
                    static_cast<unsigned long long>(handler->updates), static_cast<unsigned long long>(handler->checksum));
    } else {
        auto builder = std::make_unique<ItchBookBuilder>(kExpectedLiveOrders);
        status = replay(*builder, file, timeEach, symbols, ticksPerNs);
    }
    if (file != stdin) std::fclose(file);
    return status;
}
