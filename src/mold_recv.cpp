// MoldUDP64 feed handler: joins one or two multicast groups (feeds A and B), turns the packets into
// ITCH messages exactly once and in order (mold::Sequencer), and builds every instrument's book.
// mold_send is the other end.
//
// Usage: mold_recv [--group=239.192.0.1] [--port=26400] [--group-b=G --port-b=P] [--iface=0.0.0.0]
//                  [--core=-1] [--idle-ms=3000] [--symbols=GOOGL] [--rcvbuf=8388608] [--gap-wait-us=1000]
//
// It busy-polls the sockets, as a latency-sensitive feed handler does, until an end-of-session
// packet arrives, or nothing has arrived for --idle-ms once the feed started. Then it prints what
// arrived, what was lost, the books' consistency checks, and two latencies per packet:
//   process          decode + sequencing + book updates, timed with the TSC
//   kernel -> app    from the kernel's receive timestamp to this program reading the datagram
//                    (time in the socket queue, plus waking up if the thread wasn't running)
// --gap-wait-us is how long a gap may stay open before its messages are counted as lost: packets
// beyond it are held meanwhile, since the B feed (or a late packet) may still fill it. Too short and
// a B feed whose path is slower can't help; too long and the books wait. A gap still open when the
// program stops is counted as lost.
// --rcvbuf asks for a socket receive buffer of that many bytes (the kernel may grant less, or, on
// Linux, double it for its own bookkeeping; the granted size is printed). A large buffer absorbs a
// burst, and the packets wait in it: no loss, but the kernel -> app latency grows. A small one drops
// them, and the gaps are counted.
// Exit code 0 only if the session ended with nothing lost, nothing malformed, and the books
// consistent.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "../bench/BenchCommon.hpp"
#include "ItchBook.hpp"
#include "ItchMessages.hpp"
#include "L3OrderBook.hpp"
#include "LatencyHistogram.hpp"
#include "MoldUdp64.hpp"
#include "ThreadUtils.hpp"
#include "Tsc.hpp"
#include "UdpSocket.hpp"

namespace {

std::string argString(int argc, char** argv, const char* name, const char* fallback) {
    const std::string prefix = std::string("--") + name + "=";
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], prefix.c_str(), prefix.size()) == 0) return argv[i] + prefix.size();
    }
    return fallback;
}

struct Malformed {
    std::array<uint64_t, 6> byError{}; // indexed by mold::PacketError
    [[nodiscard]] uint64_t total() const {
        uint64_t n = 0;
        for (const uint64_t v : byError) n += v;
        return n;
    }
};

const char* errorName(size_t e) {
    constexpr std::array<const char*, 6> kNames = {"none", "too short", "block overrun", "count mismatch", "empty message",
                                                   "sequence overflow"};
    return e < kNames.size() ? kNames[e] : "?";
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--help") == 0) {
        std::printf("usage: mold_recv [--group=239.192.0.1] [--port=26400] [--group-b=G --port-b=P] [--iface=0.0.0.0]\n"
                    "                 [--core=-1] [--idle-ms=3000] [--symbols=GOOGL] [--rcvbuf=8388608] [--gap-wait-us=1000]\n");
        return 0;
    }
    const std::string iface = argString(argc, argv, "iface", "0.0.0.0");
    const std::string groupA = argString(argc, argv, "group", "239.192.0.1");
    const auto portA = static_cast<uint16_t>(bench::argInt(argc, argv, "port", 26400));
    const std::string groupB = argString(argc, argv, "group-b", "");
    const auto portB = static_cast<uint16_t>(bench::argInt(argc, argv, "port-b", portA));
    const int core = static_cast<int>(bench::argInt(argc, argv, "core", -1));
    const auto idle = std::chrono::milliseconds(bench::argInt(argc, argv, "idle-ms", 3000));
    const std::string symbols = argString(argc, argv, "symbols", "GOOGL");
    const int rcvbuf = static_cast<int>(bench::argInt(argc, argv, "rcvbuf", 8 << 20));
    const auto gapWait = std::chrono::microseconds(bench::argInt(argc, argv, "gap-wait-us", 1000));

    try {
        std::vector<UdpSocket> sockets;
        sockets.push_back(UdpSocket::receiver(groupA, portA, iface, rcvbuf));
        if (!groupB.empty()) sockets.push_back(UdpSocket::receiver(groupB, portB, iface, rcvbuf));
        const bool pinned = core >= 0 && ThreadUtils::pinThread(core);
        const double ticksPerNs = Tsc::calibrateTicksPerNs();

        std::printf("MiniHFT mold_recv: %s:%u%s, receive buffer %d bytes%s\n", groupA.c_str(), portA,
                    groupB.empty() ? "" : (", B feed " + groupB + ":" + std::to_string(portB)).c_str(),
                    sockets.front().receiveBufferBytes(), pinned ? ", pinned" : "");
        std::fflush(stdout);

        auto builder = std::make_unique<ItchBookBuilder>(size_t{1} << 16);
        mold::Sequencer sequencer;
        Malformed malformed;
        uint64_t datagrams = 0;
        uint64_t undispatched = 0; // messages MoldUDP64 delivered that ITCH refused (unknown type, wrong length)
        LatencyHistogram process;
        LatencyHistogram kernelToApp; // in ns, not ticks
        std::vector<uint8_t> buffer(65'536);

        auto toBooks = [&](uint64_t, std::span<const uint8_t> message) {
            if (!itch::dispatch(message, *builder)) ++undispatched;
        };
        std::optional<std::chrono::steady_clock::time_point> firstArrival;
        std::optional<std::chrono::steady_clock::time_point> gapSince;
        auto lastArrival = std::chrono::steady_clock::now();
        while (!sequencer.ended()) {
            bool got = false;
            for (const UdpSocket& s : sockets) {
                const std::optional<UdpSocket::Datagram> d = s.receive(buffer);
                if (!d) continue;
                const int64_t readNs = UdpSocket::realtimeNs();
                const uint64_t t0 = Tsc::read();
                got = true;
                ++datagrams;
                mold::Packet packet;
                const mold::PacketError error = mold::decode(std::span<const uint8_t>(buffer).first(d->bytes), packet);
                if (error != mold::PacketError::None) {
                    ++malformed.byError[static_cast<size_t>(error)];
                    continue;
                }
                sequencer.onPacket(packet, toBooks);
                process.record(Tsc::readOrdered() - t0);
                if (d->kernelNs > 0 && readNs >= d->kernelNs) kernelToApp.record(static_cast<uint64_t>(readNs - d->kernelNs));
            }
            const auto now = std::chrono::steady_clock::now();
            if (!sequencer.gapOpen()) {
                gapSince.reset();
            } else if (!gapSince) {
                gapSince = now;
            } else if (!got && now - *gapSince > gapWait) {
                // Only with every socket drained: if this thread was descheduled, the wait ran out
                // while the missing packet (from the B feed, say) sat in a socket queue unread.
                sequencer.giveUpGap(toBooks); // the missing packets aren't coming: count them as lost
                gapSince.reset();
            }
            if (got) {
                lastArrival = now;
                if (!firstArrival) firstArrival = now;
            } else if (firstArrival && now - lastArrival > idle) {
                break;
            }
        }
        while (sequencer.gapOpen()) sequencer.giveUpGap(toBooks); // stopped with gaps open: they're lost
        const double seconds =
            firstArrival ? std::chrono::duration<double>(lastArrival - *firstArrival).count() : 0.0;

        const mold::Sequencer::Stats& s = sequencer.stats();
        const ItchBookBuilder::Stats& b = builder->stats();
        std::printf("\n  feed      %llu datagrams, %llu packets in session, %llu heartbeats, %.3f s%s\n",
                    static_cast<unsigned long long>(datagrams), static_cast<unsigned long long>(s.packets),
                    static_cast<unsigned long long>(s.heartbeats), seconds,
                    sequencer.ended() ? ", ended" : ", NO end-of-session (stopped when idle)");
        std::printf("  messages  %llu delivered, %llu duplicates dropped (B feed or late), next expected %llu\n",
                    static_cast<unsigned long long>(s.delivered), static_cast<unsigned long long>(s.duplicates),
                    static_cast<unsigned long long>(sequencer.nextExpected()));
        std::printf("  lost      %llu messages in %llu gaps; %llu packets waited for a gap to fill\n",
                    static_cast<unsigned long long>(s.lost), static_cast<unsigned long long>(s.gaps),
                    static_cast<unsigned long long>(s.held));
        std::printf("  refused   %llu malformed datagrams, %llu from another session, %llu messages ITCH refused\n",
                    static_cast<unsigned long long>(malformed.total()), static_cast<unsigned long long>(s.otherSession),
                    static_cast<unsigned long long>(undispatched));
        for (size_t e = 1; e < malformed.byError.size(); ++e) {
            if (malformed.byError[e] > 0) {
                std::printf("            %llu %s\n", static_cast<unsigned long long>(malformed.byError[e]), errorName(e));
            }
        }
        std::printf("  books     %llu adds, %llu executions, %llu cancels, %llu deletes, %llu replaces; %llu live orders\n",
                    static_cast<unsigned long long>(b.adds), static_cast<unsigned long long>(b.executions),
                    static_cast<unsigned long long>(b.cancels), static_cast<unsigned long long>(b.deletes),
                    static_cast<unsigned long long>(b.replaces), static_cast<unsigned long long>(b.liveOrders));
        std::printf("  checks    %llu unknown references, %llu reused, %llu over-executions\n",
                    static_cast<unsigned long long>(b.unknownRefs), static_cast<unsigned long long>(b.duplicateRefs),
                    static_cast<unsigned long long>(b.overExecutions));

        for (size_t start = 0; start < symbols.size();) {
            const size_t comma = symbols.find(',', start);
            const std::string symbol = symbols.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            start = comma == std::string::npos ? symbols.size() : comma + 1;
            const int locate = builder->locateOf(symbol);
            if (locate < 0) continue;
            const L3OrderBook& book = builder->book(static_cast<uint16_t>(locate));
            const PriceLevel* bid = book.bestBid();
            const PriceLevel* ask = book.bestAsk();
            auto level = [](const PriceLevel* l) {
                if (l == nullptr) return std::string("-");
                char text[48];
                std::snprintf(text, sizeof text, "%llu @ %.4f", static_cast<unsigned long long>(l->shares), l->price / 10000.0);
                return std::string(text);
            };
            std::printf("  %-8s  bid %s  ask %s\n", symbol.c_str(), level(bid).c_str(), level(ask).c_str());
        }

        if (process.count() > 0) {
            bench::printTableHeader("per packet (ns)");
            bench::printTableRow("process", process, ticksPerNs);
            if (kernelToApp.count() > 0) bench::printTableRow("kernel -> app", kernelToApp, 1.0);
        }

        const bool clean = sequencer.ended() && s.lost == 0 && malformed.total() == 0 && undispatched == 0 &&
                           b.unknownRefs == 0 && b.overExecutions == 0;
        return clean ? 0 : 1;
    } catch (const std::system_error& e) {
        std::fprintf(stderr, "mold_recv: %s\n", e.what());
        return 1;
    }
}
