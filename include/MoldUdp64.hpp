#pragma once

// NASDAQ MoldUDP64: how ITCH messages travel over UDP multicast. Each datagram is
//
//   session (10 bytes ASCII) | sequence (u64) | count (u16) | count x [length (u16) | message]
//
// all big-endian. `sequence` numbers the packet's first message; the messages that follow are
// sequence, sequence + 1, ... A count of 0 is a heartbeat and 0xFFFF marks the end of the session;
// neither carries messages, and both carry the next sequence number. The message blocks are the
// same length-prefixed messages as NASDAQ's sample files (ItchParser.hpp).
//
// UDP can lose, duplicate and reorder datagrams, and a feed is usually sent twice, over two
// networks (A and B). Sequencer turns packets from either feed into each message exactly once,
// in order, and counts what was lost.
//
// No sockets here (those are in UdpSocket.hpp): everything below is a function of the bytes, so
// it is unit-tested and fuzzed (tests/mold_udp64_test.cpp, tests/fuzz/mold_fuzz.cpp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ItchMessages.hpp" // be16, be64, putBe16, putBe64

namespace mold {

    constexpr size_t kSessionBytes = 10;
    constexpr size_t kHeaderBytes = kSessionBytes + 8 + 2;
    constexpr uint16_t kHeartbeat = 0;
    constexpr uint16_t kEndOfSession = 0xFFFF;

    using Session = std::array<char, kSessionBytes>;

    enum class PacketError : uint8_t {
        None,
        TooShort,        // shorter than the 20-byte header
        BlockOverrun,    // a message's length runs past the end of the datagram
        CountMismatch,   // bytes left over after `count` messages
        EmptyMessage,    // a message of length 0
        SequenceOverflow // sequence + count doesn't fit in 64 bits
    };

    // A datagram that passed decode(): every message block lies inside it.
    struct Packet {
        Session session{};
        uint64_t sequence = 0;
        uint16_t count = 0;
        std::span<const uint8_t> blocks; // the length-prefixed messages, `count` of them

        [[nodiscard]] bool heartbeat() const { return count == kHeartbeat; }
        [[nodiscard]] bool endOfSession() const { return count == kEndOfSession; }
        [[nodiscard]] uint16_t messages() const { return count == kEndOfSession ? 0 : count; }
    };

    // Validates the whole datagram before anything is delivered, so a malformed packet delivers
    // nothing: the Sequencer then sees it as lost, and the gap is counted, instead of half a
    // packet arriving and the sequence numbers drifting.
    [[nodiscard]] inline PacketError decode(std::span<const uint8_t> datagram, Packet& out) {
        if (datagram.size() < kHeaderBytes) return PacketError::TooShort;
        Packet p;
        std::memcpy(p.session.data(), datagram.data(), kSessionBytes);
        p.sequence = itch::be64(datagram.data() + kSessionBytes);
        p.count = itch::be16(datagram.data() + kSessionBytes + 8);
        p.blocks = datagram.subspan(kHeaderBytes);
        // Untrusted: sequence + count must not wrap, or the next expected number would jump back.
        if (p.sequence > std::numeric_limits<uint64_t>::max() - p.messages()) return PacketError::SequenceOverflow;

        size_t offset = 0;
        for (uint16_t i = 0; i < p.messages(); ++i) {
            if (p.blocks.size() - offset < 2) return PacketError::BlockOverrun;
            const size_t len = itch::be16(p.blocks.data() + offset);
            if (len == 0) return PacketError::EmptyMessage;
            if (p.blocks.size() - offset - 2 < len) return PacketError::BlockOverrun;
            offset += 2 + len;
        }
        if (offset != p.blocks.size()) return PacketError::CountMismatch;
        out = p;
        return PacketError::None;
    }

    // Calls fn(sequenceNumber, message) for each message of a decoded packet, in order.
    template <typename Fn>
    void forEachMessage(const Packet& p, Fn&& fn) {
        size_t offset = 0;
        for (uint16_t i = 0; i < p.messages(); ++i) {
            const size_t len = itch::be16(p.blocks.data() + offset);
            fn(p.sequence + i, p.blocks.subspan(offset + 2, len));
            offset += 2 + len;
        }
    }

    // Builds MoldUDP64 datagrams in a caller's buffer: the sender's side, and the tests'.
    class PacketWriter {
    public:
        PacketWriter(std::span<uint8_t> buffer, std::string_view session) : buffer_(buffer) {
            session_.fill(' ');
            std::memcpy(session_.data(), session.data(), session.size() < kSessionBytes ? session.size() : kSessionBytes);
        }

        // Starts a packet whose first message will have sequence number `sequence`.
        void begin(uint64_t sequence) {
            sequence_ = sequence;
            count_ = 0;
            size_ = kHeaderBytes;
        }

        // Appends a message; false (and nothing appended) if it doesn't fit, or the packet already
        // holds the most messages a count can describe.
        [[nodiscard]] bool add(std::span<const uint8_t> message) {
            if (message.empty() || message.size() > 0xFFFF || count_ == kEndOfSession - 1) return false;
            if (buffer_.size() - size_ < 2 + message.size()) return false;
            itch::putBe16(buffer_.data() + size_, static_cast<uint16_t>(message.size()));
            std::memcpy(buffer_.data() + size_ + 2, message.data(), message.size());
            size_ += 2 + message.size();
            ++count_;
            return true;
        }

        [[nodiscard]] uint16_t count() const { return count_; }

        // The finished datagram, valid until the next begin().
        [[nodiscard]] std::span<const uint8_t> finish() { return finishWithCount(count_); }

        // A heartbeat or end-of-session datagram: no messages, `next` is the next sequence number.
        [[nodiscard]] std::span<const uint8_t> control(uint64_t next, uint16_t count) {
            begin(next);
            return finishWithCount(count);
        }

    private:
        std::span<const uint8_t> finishWithCount(uint16_t count) {
            std::memcpy(buffer_.data(), session_.data(), kSessionBytes);
            itch::putBe64(buffer_.data() + kSessionBytes, sequence_);
            itch::putBe16(buffer_.data() + kSessionBytes + 8, count);
            return buffer_.first(size_);
        }

        std::span<uint8_t> buffer_;
        Session session_{};
        uint64_t sequence_ = 1;
        uint16_t count_ = 0;
        size_t size_ = kHeaderBytes;
    };

    // Turns packets from one or two feeds into each message exactly once, in sequence order.
    //
    // - A message whose number was already delivered is dropped: a duplicate from the other
    //   feed, a retransmission, or a stale packet arriving late.
    // - A packet that starts beyond the next expected number is held, not delivered: the
    //   missing packets may still come, from the other feed (whose path may be slower), or out of
    //   order. When they do, the held packets follow in order. Skipping ahead at once would make
    //   the B feed useless whenever A's next packet is read before B's copy of the missing one.
    // - Only giveUpGap() declares a gap lost: the oldest one. Its missing messages are counted and
    //   delivery continues from the held packets, up to the next gap if there is one; that gap gets
    //   its own wait, since the packets beyond it may have only just arrived. How long to wait is the caller's policy
    //   (mold_recv: --gap-wait-us), since it depends on time; this class only sees packets. When
    //   more than maxHeld packets are held, the oldest gap is given up without waiting. (A
    //   production receiver would also request the range from a retransmission server.)
    // - Heartbeats and the end-of-session packet carry the next sequence number, so they reveal
    //   a gap at the tail of the feed even when no data follows.
    // - The first packet fixes the session; packets from another session are counted and dropped.
    //
    // The in-order path copies nothing; held packets are copied, since the receive buffer they
    // came in is reused for the next datagram.
    class Sequencer {
    public:
        struct Stats {
            uint64_t packets = 0;
            uint64_t heartbeats = 0;
            uint64_t delivered = 0;
            uint64_t duplicates = 0;   // messages dropped because their number was already delivered
            uint64_t held = 0;         // packets that arrived beyond a gap and waited
            uint64_t gaps = 0;         // gaps given up on
            uint64_t lost = 0;         // messages in those gaps
            uint64_t otherSession = 0; // packets dropped for carrying another session
        };

        explicit Sequencer(uint64_t firstSequence = 1, size_t maxHeld = 4096) : next_(firstSequence), maxHeld_(maxHeld) {}

        // deliver(sequenceNumber, message) is called for each new message, in order.
        template <typename Fn>
        void onPacket(const Packet& p, Fn&& deliver) {
            if (!haveSession_) {
                session_ = p.session;
                haveSession_ = true;
            } else if (p.session != session_) {
                ++stats_.otherSession;
                return;
            }
            ++stats_.packets;
            const uint64_t end = p.sequence + p.messages(); // decode() checked this doesn't wrap
            if (end > known_) known_ = end;
            if (p.heartbeat()) ++stats_.heartbeats;
            if (p.endOfSession()) endOfSession_ = true;
            if (p.messages() == 0) {
                finishIfEnded();
                return;
            }
            if (p.sequence > next_) {
                if (held_.size() >= maxHeld_) giveUpGap(deliver);
                if (p.sequence > next_) { // still beyond: hold it
                    hold(p);
                    return;
                }
            }
            deliverNew(p.sequence, p.blocks, p.messages(), deliver);
            drainHeld(deliver);
            finishIfEnded();
        }

        // Declares the oldest open gap lost: counts its missing messages and continues from the
        // first held packet (or, if none is held, from the next number a heartbeat announced).
        // Delivery stops at the next gap, if any; gapOpen() then stays true until that one is
        // filled or given up too.
        template <typename Fn>
        void giveUpGap(Fn&& deliver) {
            const uint64_t target = held_.empty() ? known_ : held_.front().sequence;
            if (target > next_) {
                ++stats_.gaps;
                stats_.lost += target - next_;
                next_ = target;
            }
            drainHeld(deliver);
            finishIfEnded();
        }

        // Messages are known to be missing: a packet is held, or a heartbeat announced more.
        [[nodiscard]] bool gapOpen() const { return known_ > next_; }
        [[nodiscard]] uint64_t nextExpected() const { return next_; }
        [[nodiscard]] bool ended() const { return ended_; } // end of session, with nothing missing before it
        [[nodiscard]] const Stats& stats() const { return stats_; }

    private:
        struct Held {
            uint64_t sequence;
            uint16_t count;
            std::vector<uint8_t> blocks;
        };

        template <typename Fn>
        void deliverNew(uint64_t sequence, std::span<const uint8_t> blocks, uint16_t count, Fn& deliver) {
            Packet view;
            view.sequence = sequence;
            view.count = count;
            view.blocks = blocks;
            forEachMessage(view, [&](uint64_t seq, std::span<const uint8_t> message) {
                if (seq < next_) {
                    ++stats_.duplicates;
                    return;
                }
                ++next_;
                ++stats_.delivered;
                deliver(seq, message);
            });
        }

        void hold(const Packet& p) {
            ++stats_.held;
            // Kept sorted by sequence; gaps are rare and short, so a vector with an insert beats a map.
            auto at = held_.begin();
            while (at != held_.end() && at->sequence < p.sequence) ++at;
            if (at != held_.end() && at->sequence == p.sequence && at->count >= p.count) {
                stats_.duplicates += p.count; // the same packet from the other feed, already held
                return;
            }
            held_.insert(at, Held{p.sequence, p.count, std::vector<uint8_t>(p.blocks.begin(), p.blocks.end())});
        }

        template <typename Fn>
        void drainHeld(Fn& deliver) {
            while (!held_.empty() && held_.front().sequence <= next_) {
                const Held h = std::move(held_.front());
                held_.erase(held_.begin());
                deliverNew(h.sequence, h.blocks, h.count, deliver);
            }
        }

        void finishIfEnded() {
            if (endOfSession_ && !gapOpen()) ended_ = true;
        }

        uint64_t next_;
        uint64_t known_ = 0; // one past the highest sequence number any packet has announced
        size_t maxHeld_;
        std::vector<Held> held_; // packets beyond a gap, by sequence
        Session session_{};
        bool haveSession_ = false;
        bool endOfSession_ = false;
        bool ended_ = false;
        Stats stats_;
    };

} // namespace mold
