#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ItchMessages.hpp"
#include "MoldUdp64.hpp"

namespace {

using Bytes = std::vector<uint8_t>;

// A message whose first byte is `tag`, `len` bytes long. MoldUDP64 doesn't look inside messages.
Bytes message(uint8_t tag, size_t len = 5) {
    Bytes m(len, 0);
    m[0] = tag;
    return m;
}

// One datagram with messages tag, tag + 1, ... starting at `sequence`.
Bytes packet(uint64_t sequence, uint8_t firstTag, uint16_t count, const char* session = "SESSION1") {
    std::array<uint8_t, 1400> buffer{};
    mold::PacketWriter writer(buffer, session);
    writer.begin(sequence);
    for (uint16_t i = 0; i < count; ++i) {
        const Bytes m = message(static_cast<uint8_t>(firstTag + i));
        EXPECT_TRUE(writer.add(m));
    }
    const std::span<const uint8_t> out = writer.finish();
    return {out.begin(), out.end()};
}

Bytes control(uint64_t next, uint16_t count, const char* session = "SESSION1") {
    std::array<uint8_t, 64> buffer{};
    mold::PacketWriter writer(buffer, session);
    const std::span<const uint8_t> out = writer.control(next, count);
    return {out.begin(), out.end()};
}

mold::Packet decoded(const Bytes& datagram) {
    mold::Packet p;
    EXPECT_EQ(mold::decode(datagram, p), mold::PacketError::None);
    return p;
}

using Delivered = std::vector<std::pair<uint64_t, uint8_t>>;

// Feeds datagrams to a sequencer and returns the (sequence, first byte) of each message delivered.
Delivered feed(mold::Sequencer& seq, const std::vector<Bytes>& datagrams) {
    Delivered out;
    for (const Bytes& d : datagrams) {
        seq.onPacket(decoded(d), [&](uint64_t n, std::span<const uint8_t> m) { out.emplace_back(n, m[0]); });
    }
    return out;
}

Delivered giveUp(mold::Sequencer& seq) {
    Delivered out;
    seq.giveUpGap([&](uint64_t n, std::span<const uint8_t> m) { out.emplace_back(n, m[0]); });
    return out;
}

} // namespace

TEST(MoldUdp64, WriterAndDecoderAgree) {
    const Bytes d = packet(1000, 'a', 3);
    ASSERT_EQ(d.size(), mold::kHeaderBytes + size_t{3} * (2 + 5));
    const mold::Packet p = decoded(d);
    EXPECT_EQ(std::string(p.session.data(), p.session.size()), "SESSION1  "); // padded with spaces
    EXPECT_EQ(p.sequence, 1000u);
    EXPECT_EQ(p.count, 3u);
    std::vector<std::pair<uint64_t, uint8_t>> seen;
    mold::forEachMessage(p, [&](uint64_t n, std::span<const uint8_t> m) {
        EXPECT_EQ(m.size(), 5u);
        seen.emplace_back(n, m[0]);
    });
    EXPECT_EQ(seen, (Delivered{{1000, 'a'}, {1001, 'b'}, {1002, 'c'}}));
}

TEST(MoldUdp64, WriterRefusesWhatDoesntFit) {
    std::array<uint8_t, mold::kHeaderBytes + 2 + 5> buffer{};
    mold::PacketWriter writer(buffer, "S");
    writer.begin(1);
    EXPECT_TRUE(writer.add(message('a')));
    EXPECT_FALSE(writer.add(message('b'))); // no room left: nothing appended
    EXPECT_FALSE(writer.add({}));           // an empty message can't be framed
    EXPECT_EQ(writer.count(), 1u);
    EXPECT_EQ(decoded({writer.finish().begin(), writer.finish().end()}).count, 1u);
}

TEST(MoldUdp64, HeartbeatAndEndOfSessionCarryNoMessages) {
    const mold::Packet hb = decoded(control(42, mold::kHeartbeat));
    EXPECT_TRUE(hb.heartbeat());
    EXPECT_EQ(hb.messages(), 0u);
    const mold::Packet end = decoded(control(42, mold::kEndOfSession));
    EXPECT_TRUE(end.endOfSession());
    EXPECT_EQ(end.messages(), 0u); // 0xFFFF is a marker, not a count
}

// Every way a datagram can lie about its contents is refused, and nothing is delivered from it.
TEST(MoldUdp64, MalformedDatagramsAreRefusedWhole) {
    mold::Packet p;
    const Bytes good = packet(1, 'a', 2);

    EXPECT_EQ(mold::decode(std::span<const uint8_t>(good).first(mold::kHeaderBytes - 1), p), mold::PacketError::TooShort);

    Bytes cut = good;
    cut.pop_back(); // the last message ends past the datagram
    EXPECT_EQ(mold::decode(cut, p), mold::PacketError::BlockOverrun);

    Bytes extra = good;
    extra.push_back(0); // a byte that no message accounts for
    EXPECT_EQ(mold::decode(extra, p), mold::PacketError::CountMismatch);

    Bytes moreThanSent = good;
    itch::putBe16(moreThanSent.data() + mold::kSessionBytes + 8, 3); // claims 3 messages, holds 2
    EXPECT_EQ(mold::decode(moreThanSent, p), mold::PacketError::BlockOverrun);

    Bytes lyingLength = good;
    itch::putBe16(lyingLength.data() + mold::kHeaderBytes, 0x7FFF); // first message claims 32 KB
    EXPECT_EQ(mold::decode(lyingLength, p), mold::PacketError::BlockOverrun);

    Bytes empty = good;
    itch::putBe16(empty.data() + mold::kHeaderBytes, 0);
    EXPECT_EQ(mold::decode(empty, p), mold::PacketError::EmptyMessage);

    // sequence + count past 2^64: the next expected number would wrap to the start.
    const Bytes wrap = packet(std::numeric_limits<uint64_t>::max() - 1, 'a', 2);
    EXPECT_EQ(mold::decode(wrap, p), mold::PacketError::SequenceOverflow);
    const Bytes fits = packet(std::numeric_limits<uint64_t>::max() - 2, 'a', 2);
    EXPECT_EQ(mold::decode(fits, p), mold::PacketError::None);
}

TEST(MoldUdp64Sequencer, DeliversInOrderPacketsOnce) {
    mold::Sequencer seq;
    const auto out = feed(seq, {packet(1, 'a', 2), packet(3, 'c', 2)});
    EXPECT_EQ(out, (Delivered{{1, 'a'}, {2, 'b'}, {3, 'c'}, {4, 'd'}}));
    EXPECT_EQ(seq.nextExpected(), 5u);
    EXPECT_EQ(seq.stats().delivered, 4u);
    EXPECT_EQ(seq.stats().gaps, 0u);
}

// The same packets arriving on both feeds: each message is delivered once, from whichever copy
// came first.
TEST(MoldUdp64Sequencer, ArbitratesBetweenFeedsAAndB) {
    mold::Sequencer seq;
    const auto out = feed(seq, {packet(1, 'a', 2), packet(1, 'a', 2), packet(3, 'c', 1), packet(3, 'c', 1)});
    EXPECT_EQ(out.size(), 3u);
    EXPECT_EQ(seq.stats().duplicates, 3u);
    EXPECT_EQ(seq.stats().gaps, 0u);
}

// Feed A loses a packet that feed B still delivers: no gap.
TEST(MoldUdp64Sequencer, OtherFeedFillsALostPacket) {
    mold::Sequencer seq;
    const auto out = feed(seq, {packet(1, 'a', 2), /* A lost 3-4 */ packet(3, 'c', 2) /* from B */, packet(3, 'c', 2) /* A's late copy */});
    EXPECT_EQ(out.size(), 4u);
    EXPECT_EQ(seq.stats().gaps, 0u);
}

// The packet that failed the loopback test: feed A drops packet 3-4, and A's next packet is read
// before B's copy of 3-4. Holding packet 5 until 3-4 arrives keeps the B feed useful.
TEST(MoldUdp64Sequencer, AHeldPacketWaitsForTheOtherFeedToFillTheGap) {
    mold::Sequencer seq;
    EXPECT_EQ(feed(seq, {packet(1, 'a', 2), packet(5, 'e', 2) /* A, read first */}),
              (Delivered{{1, 'a'}, {2, 'b'}}));
    EXPECT_TRUE(seq.gapOpen());
    EXPECT_EQ(feed(seq, {packet(3, 'c', 2) /* B's copy */}), (Delivered{{3, 'c'}, {4, 'd'}, {5, 'e'}, {6, 'f'}}));
    EXPECT_FALSE(seq.gapOpen());
    EXPECT_EQ(seq.stats().held, 1u);
    EXPECT_EQ(seq.stats().gaps, 0u);
    EXPECT_EQ(feed(seq, {packet(5, 'e', 2) /* B's copy of the held one */}), Delivered{});
    EXPECT_EQ(seq.stats().duplicates, 2u);
}

TEST(MoldUdp64Sequencer, GivingUpCountsTheGapAndResumes) {
    mold::Sequencer seq;
    EXPECT_EQ(feed(seq, {packet(1, 'a', 2), packet(6, 'f', 1)}), (Delivered{{1, 'a'}, {2, 'b'}})); // 3-5 missing
    EXPECT_EQ(seq.stats().gaps, 0u); // not yet: they may still come
    EXPECT_EQ(giveUp(seq), (Delivered{{6, 'f'}}));
    EXPECT_EQ(seq.stats().gaps, 1u);
    EXPECT_EQ(seq.stats().lost, 3u);
    EXPECT_FALSE(seq.gapOpen());
    // The missing packet arriving after that is stale, not a refill: dropped.
    EXPECT_EQ(feed(seq, {packet(3, 'c', 3)}), Delivered{});
    EXPECT_EQ(seq.stats().duplicates, 3u);
    EXPECT_EQ(giveUp(seq), Delivered{}); // nothing open: nothing counted
    EXPECT_EQ(seq.stats().gaps, 1u);
}

// Found by the fuzzer: with two gaps open, giving up closes only the oldest. The packets beyond the
// second may have only just arrived, so that gap gets its own wait.
TEST(MoldUdp64Sequencer, GivingUpClosesOnlyTheOldestGap) {
    mold::Sequencer seq;
    EXPECT_EQ(feed(seq, {packet(1, 'a', 1), packet(3, 'c', 1), packet(6, 'f', 1)}), (Delivered{{1, 'a'}})); // 2, 4-5 missing
    EXPECT_EQ(giveUp(seq), (Delivered{{3, 'c'}}));
    EXPECT_TRUE(seq.gapOpen());
    EXPECT_EQ(seq.stats().lost, 1u);
    EXPECT_EQ(giveUp(seq), (Delivered{{6, 'f'}}));
    EXPECT_FALSE(seq.gapOpen());
    EXPECT_EQ(seq.stats().gaps, 2u);
    EXPECT_EQ(seq.stats().lost, 3u);
}

TEST(MoldUdp64Sequencer, HeldPacketsAreDeliveredInSequenceOrder) {
    mold::Sequencer seq;
    EXPECT_EQ(feed(seq, {packet(1, 'a', 1), packet(5, 'e', 1), packet(3, 'c', 1), packet(4, 'd', 1)}), (Delivered{{1, 'a'}}));
    EXPECT_EQ(feed(seq, {packet(2, 'b', 1)}), (Delivered{{2, 'b'}, {3, 'c'}, {4, 'd'}, {5, 'e'}}));
    EXPECT_EQ(seq.stats().held, 3u);
}

// A feed that never recovers can't make the sequencer hold packets without bound.
TEST(MoldUdp64Sequencer, TooManyHeldPacketsGiveUpTheOldestGap) {
    mold::Sequencer seq(1, 2);
    EXPECT_EQ(feed(seq, {packet(1, 'a', 1), packet(3, 'c', 1), packet(4, 'd', 1)}), (Delivered{{1, 'a'}})); // 2 missing
    EXPECT_EQ(feed(seq, {packet(5, 'e', 1)}), (Delivered{{3, 'c'}, {4, 'd'}, {5, 'e'}})); // a third held: give up on 2
    EXPECT_EQ(seq.stats().gaps, 1u);
    EXPECT_EQ(seq.stats().lost, 1u);
}

TEST(MoldUdp64Sequencer, EndOfSessionWaitsForAnOpenGap) {
    mold::Sequencer seq;
    feed(seq, {packet(1, 'a', 1), control(4, mold::kEndOfSession)}); // 2-3 still missing
    EXPECT_FALSE(seq.ended());
    feed(seq, {packet(2, 'b', 2)});
    EXPECT_TRUE(seq.ended());
    EXPECT_EQ(seq.stats().gaps, 0u);
}

// A packet that overlaps what was delivered (a retransmission, or feeds with different packet
// boundaries) delivers only its new messages.
TEST(MoldUdp64Sequencer, DeliversOnlyTheNewPartOfAnOverlappingPacket) {
    mold::Sequencer seq;
    const auto out = feed(seq, {packet(1, 'a', 3), packet(2, 'b', 4)});
    EXPECT_EQ(out, (Delivered{{1, 'a'}, {2, 'b'}, {3, 'c'}, {4, 'd'}, {5, 'e'}}));
    EXPECT_EQ(seq.stats().duplicates, 2u);
}

// The last data packet is lost and the feed goes quiet: only the heartbeat's sequence number
// reveals it.
TEST(MoldUdp64Sequencer, HeartbeatRevealsAGapAtTheTail) {
    mold::Sequencer seq;
    feed(seq, {packet(1, 'a', 2), /* 3-4 lost */ control(5, mold::kHeartbeat)});
    EXPECT_EQ(seq.stats().heartbeats, 1u);
    EXPECT_TRUE(seq.gapOpen());
    giveUp(seq);
    EXPECT_EQ(seq.stats().gaps, 1u);
    EXPECT_EQ(seq.stats().lost, 2u);
    EXPECT_EQ(seq.nextExpected(), 5u);
}

TEST(MoldUdp64Sequencer, EndOfSessionIsReported) {
    mold::Sequencer seq;
    feed(seq, {packet(1, 'a', 1)});
    EXPECT_FALSE(seq.ended());
    feed(seq, {control(2, mold::kEndOfSession)});
    EXPECT_TRUE(seq.ended());
    EXPECT_EQ(seq.stats().gaps, 0u);
}

TEST(MoldUdp64Sequencer, DropsPacketsFromAnotherSession) {
    mold::Sequencer seq;
    const auto out = feed(seq, {packet(1, 'a', 1, "TODAY"), packet(2, 'b', 1, "YESTERDAY"), packet(2, 'b', 1, "TODAY")});
    EXPECT_EQ(out.size(), 2u);
    EXPECT_EQ(seq.stats().otherSession, 1u);
    EXPECT_EQ(seq.stats().gaps, 0u);
}
