// libFuzzer target: a sequence of arbitrary datagrams through mold::decode and mold::Sequencer, and
// what they deliver through itch::dispatch into a book builder: the receive path of mold_recv,
// with the network replaced by the fuzzer.
//
// The input is split into datagrams, each preceded by its length as a 2-byte big-endian integer
// (the rest of the input when that runs short), so one input is a whole stretch of feed: gaps,
// duplicates and stale packets included. A 1-byte datagram stands for the receiver giving up on
// the open gap (mold_recv does that after --gap-wait-us).
//
//   cmake --preset clang-fuzz && cmake --build --preset clang-fuzz
//   mkdir mold_corpus && cp tests/fuzz/mold_corpus/* mold_corpus/
//   ./build/clang-fuzz/mold_fuzz -max_total_time=60 mold_corpus/
// Once a crash is fixed, add its input to tests/fuzz/mold_corpus/ so every run replays it.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "ItchBook.hpp"
#include "ItchMessages.hpp"
#include "MoldUdp64.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // One builder for the whole run: allocating 65,536 books for every input would be too slow.
    static auto builder = std::make_unique<ItchBookBuilder>(1024);
    mold::Sequencer sequencer;
    uint64_t last = 0;
    bool delivered = false;

    for (size_t at = 0; at < size;) {
        size_t len = size - at;
        if (len >= 2) {
            const size_t claimed = itch::be16(data + at);
            at += 2;
            len = claimed < size - at ? claimed : size - at;
        } else {
            at += len;
            len = 0;
        }
        // Each datagram in its own allocation of exactly its size, as recvmsg leaves it: a read
        // past its end then leaves the allocation, where ASan reports it, instead of landing in
        // the next datagram of the input.
        const std::vector<uint8_t> datagram(data + at, data + at + len);
        at += len;

        auto deliver = [&](uint64_t seq, std::span<const uint8_t> message) {
            // Exactly once, in order: every delivered number is beyond the previous one, and the
            // sequencer expects the next one.
            if (delivered && seq <= last) __builtin_trap();
            if (sequencer.nextExpected() != seq + 1) __builtin_trap();
            if (message.empty()) __builtin_trap(); // decode refuses empty messages
            last = seq;
            delivered = true;
            (void)itch::dispatch(message, *builder);
        };
        if (datagram.size() == 1) {
            // Giving up makes progress, and repeating it closes every gap (mold_recv does that at
            // the end). Each call gives up only the oldest gap.
            for (int round = 0; sequencer.gapOpen(); ++round) {
                const uint64_t before = sequencer.nextExpected();
                sequencer.giveUpGap(deliver);
                if (sequencer.nextExpected() <= before || round > 10'000) __builtin_trap();
            }
            continue;
        }
        mold::Packet packet;
        if (mold::decode(datagram, packet) != mold::PacketError::None) continue;
        sequencer.onPacket(packet, deliver);
    }
    const mold::Sequencer::Stats& s = sequencer.stats();
    if (delivered && sequencer.nextExpected() != last + 1 && s.gaps == 0) __builtin_trap(); // only a gap moves it further
    return 0;
}
