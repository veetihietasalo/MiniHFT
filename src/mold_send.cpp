// Feed simulator: replays an ITCH 5.0 file (the length-prefixed format of NASDAQ's sample files and
// gen_itch) as a MoldUDP64 multicast feed, the way the exchange sends it. mold_recv is the other end.
//
// Usage: mold_send <file | -> [--group=239.192.0.1] [--port=26400] [--group-b=G --port-b=P]
//                  [--iface=0.0.0.0] [--ttl=0] [--max-bytes=1400] [--pps=0]
//                  [--drop-a=0] [--repeat=1] [--session=MINIHFT]
//
//   --max-bytes  largest datagram payload. 1400 keeps each packet inside a 1500-byte Ethernet frame:
//                an IP datagram split into fragments is lost if any fragment is.
//   --pps        packets per second; 0 sends as fast as the kernel takes them. A receiver that falls
//                behind then loses datagrams when its socket buffer fills, and reports the gaps.
//   --group-b    also send every packet to a second group (the B feed), as exchanges do.
//   --drop-a=N   skip every Nth packet on feed A (not B): gaps that the B feed should fill.
//   --repeat=N   send the file N times as one session, with sequence numbers running on. Order
//                references repeat too, so a book builder will report them as reused.
//   --ttl        0 keeps packets on this host, 1 on the local network.
// The session ends with an end-of-session packet, sent three times because UDP may lose one.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "ItchParser.hpp"
#include "MoldUdp64.hpp"
#include "UdpSocket.hpp"

namespace {

std::string argString(int argc, char** argv, const char* name, const char* fallback) {
    const std::string prefix = std::string("--") + name + "=";
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], prefix.c_str(), prefix.size()) == 0) return argv[i] + prefix.size();
    }
    return fallback;
}

long long argInt(int argc, char** argv, const char* name, long long fallback) {
    const std::string value = argString(argc, argv, name, "");
    return value.empty() ? fallback : std::stoll(value);
}

struct Feed {
    std::string group;
    uint16_t port;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0) {
        std::printf("usage: mold_send <file | -> [--group=239.192.0.1] [--port=26400] [--group-b=G --port-b=P]\n"
                    "                 [--iface=0.0.0.0] [--ttl=0] [--max-bytes=1400] [--pps=0] [--drop-a=0]\n"
                    "                 [--repeat=1] [--session=MINIHFT]\n");
        return argc < 2 ? 2 : 0;
    }
    const std::string path = argv[1];
    const Feed feedA{argString(argc, argv, "group", "239.192.0.1"), static_cast<uint16_t>(argInt(argc, argv, "port", 26400))};
    const std::string groupB = argString(argc, argv, "group-b", "");
    const Feed feedB{groupB, static_cast<uint16_t>(argInt(argc, argv, "port-b", feedA.port))};
    const auto maxBytes = static_cast<size_t>(argInt(argc, argv, "max-bytes", 1400));
    const long long pps = argInt(argc, argv, "pps", 0);
    const long long dropA = argInt(argc, argv, "drop-a", 0);
    const long long repeat = argInt(argc, argv, "repeat", 1);
    if (maxBytes < mold::kHeaderBytes + 2 + 64 || maxBytes > 65'000) {
        std::fprintf(stderr, "--max-bytes must be between %zu and 65000\n", mold::kHeaderBytes + 2 + 64);
        return 2;
    }

    try {
        const UdpSocket socket = UdpSocket::sender(argString(argc, argv, "iface", "0.0.0.0"),
                                                   static_cast<int>(argInt(argc, argv, "ttl", 0)));
        std::vector<uint8_t> buffer(maxBytes);
        mold::PacketWriter writer(buffer, argString(argc, argv, "session", "MINIHFT"));

        uint64_t packets = 0;
        uint64_t messages = 0;
        uint64_t dropped = 0;
        uint64_t refused = 0;
        uint64_t nextSequence = 1;
        const auto interval = pps > 0 ? std::chrono::nanoseconds(1'000'000'000 / pps) : std::chrono::nanoseconds(0);
        auto due = std::chrono::steady_clock::now();
        const auto start = due;

        auto send = [&](std::span<const uint8_t> datagram, bool mayDrop) {
            if (interval.count() > 0) {
                due += interval;
                std::this_thread::sleep_until(due);
            }
            ++packets;
            if (mayDrop && dropA > 0 && packets % static_cast<uint64_t>(dropA) == 0) {
                ++dropped;
            } else if (!socket.sendTo(datagram, feedA.group, feedA.port)) {
                ++refused;
            }
            if (!feedB.group.empty() && !socket.sendTo(datagram, feedB.group, feedB.port)) ++refused;
        };
        auto flush = [&] {
            if (writer.count() == 0) return;
            const uint16_t count = writer.count();
            send(writer.finish(), true);
            nextSequence += count;
            writer.begin(nextSequence);
        };

        writer.begin(nextSequence);
        for (long long pass = 0; pass < repeat; ++pass) {
            std::FILE* file = path == "-" ? stdin : std::fopen(path.c_str(), "rb");
            if (file == nullptr) {
                std::fprintf(stderr, "cannot open %s\n", path.c_str());
                return 1;
            }
            ItchReader reader(file);
            for (std::span<const uint8_t> m = reader.next(); !m.empty(); m = reader.next()) {
                if (!writer.add(m)) {
                    flush();
                    if (!writer.add(m)) {
                        std::fprintf(stderr, "a %zu-byte message doesn't fit in --max-bytes=%zu\n", m.size(), maxBytes);
                        return 1;
                    }
                }
                ++messages;
            }
            if (file != stdin) std::fclose(file);
        }
        flush();
        for (int i = 0; i < 3; ++i) send(writer.control(nextSequence, mold::kEndOfSession), false);

        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("mold_send: %llu messages in %llu packets to %s:%u%s, %.3f s\n",
                    static_cast<unsigned long long>(messages), static_cast<unsigned long long>(packets),
                    feedA.group.c_str(), feedA.port, feedB.group.empty() ? "" : (" and " + feedB.group).c_str(), seconds);
        if (dropped > 0) std::printf("  dropped %llu packets on feed A on purpose (--drop-a)\n", static_cast<unsigned long long>(dropped));
        if (refused > 0) std::printf("  the kernel refused %llu datagrams\n", static_cast<unsigned long long>(refused));
        return refused > 0 ? 1 : 0;
    } catch (const std::system_error& e) {
        std::fprintf(stderr, "mold_send: %s\n", e.what());
        return 1;
    }
}
