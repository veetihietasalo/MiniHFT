#pragma once

// Two-thread stress test, shared by the unit tests, the schedule stress program
// (tests/spsc_stress.cpp) and the broken-queue demos.
//
// Each message fills one 64-byte cache line: a sequence number, six payload words derived
// from it, and a checksum. A consumer that reads a slot before the producer's writes are
// visible sees a wrong sequence number, stale payload words or a checksum that doesn't match.
//
// ThreadSanitizer only checks the interleavings that actually happen, so the test can run
// under several schedules (StressSchedule):
//   YieldWhenBlocked  yield whenever the queue is full or empty (the default; the unit tests)
//   Tight             busy-spin on a full or empty queue: the fastest hand-offs
//   Random            seeded random delays between the steps of each hand-off, on both sides:
//                     short spins, yields, and now and then a stall long enough for the other
//                     side to fill or drain a 1024-slot queue
// Pair any of them with a tiny queue (2 or 4 slots) to make it wrap, fill and empty constantly.

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

struct StressMessage {
    uint64_t seq;
    uint64_t payload[6];
    uint64_t checksum;
};
static_assert(sizeof(StressMessage) == 64, "one message per cache line");

struct StressResult {
    uint64_t received = 0;
    uint64_t badSequence = 0;
    uint64_t badPayload = 0;
    uint64_t badChecksum = 0;

    bool clean() const { return badSequence == 0 && badPayload == 0 && badChecksum == 0; }
};

enum class StressSchedule : uint8_t { YieldWhenBlocked, Tight, Random };

// splitmix64 finalizer: a cheap, well-mixed function of (seq, k).
inline uint64_t stressWord(uint64_t seq, unsigned k) {
    uint64_t x = seq * 8 + k + 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

inline uint64_t stressChecksum(const StressMessage& m) {
    uint64_t sum = m.seq;
    for (uint64_t word : m.payload) sum = sum * 31 + word;
    return sum;
}

// The delays one thread inserts into its side of a hand-off. Each thread owns its own, seeded
// differently, so the two sides drift against each other.
class SchedulePerturber {
public:
    SchedulePerturber(StressSchedule schedule, uint64_t seed) : schedule_(schedule), state_(seed) {}

    // Between two steps of a hand-off (e.g. after writing a slot, before publishing it).
    // Random: 7 times in 8 nothing, otherwise a yield or a spin of up to 2 us, and about once
    // in 4096 steps a stall of up to 2 ms instead. Under TSan a message takes 1-2 us, so a
    // stall lets the other side fill or drain even a 1024-slot queue.
    void step() {
        if (schedule_ != StressSchedule::Random) return;
        const uint64_t r = next();
        const auto upTo2000 = static_cast<int64_t>((r >> 12) % 2000); // independent of the bits tested below
        if ((r & 4095) == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(upTo2000));
        } else if ((r & 7) == 0) {
            if ((r & 8) != 0) {
                std::this_thread::yield();
            } else {
                spinFor(std::chrono::nanoseconds(upTo2000));
            }
        }
    }

    // Each time the queue was full (producer) or empty (consumer).
    void blocked() {
        if (schedule_ == StressSchedule::Tight) {
            // Spin, but yield now and then: if the machine is oversubscribed and the other
            // thread is waiting for this core, a pure spin would stall until preempted.
            if (++blockedSpins_ % kTightSpinsPerYield == 0) std::this_thread::yield();
            return;
        }
        std::this_thread::yield();
    }

private:
    static constexpr uint64_t kTightSpinsPerYield = 4096;

    uint64_t next() { // splitmix64
        uint64_t x = (state_ += 0x9e3779b97f4a7c15ULL);
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }

    // A clock read can't be optimized away, unlike an empty loop.
    static void spinFor(std::chrono::nanoseconds duration) {
        const auto until = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < until) {}
    }

    StressSchedule schedule_;
    uint64_t state_;
    uint64_t blockedSpins_ = 0;
};

template <typename Queue>
StressResult runSpscStress(uint64_t count, StressSchedule schedule = StressSchedule::YieldWhenBlocked,
                           uint64_t seed = 0) {
    auto queue = std::make_unique<Queue>();
    StressResult result;

    std::thread producer([&] {
        SchedulePerturber perturb(schedule, seed ^ 0x5bd1e995ULL);
        for (uint64_t i = 0; i < count; ++i) {
            perturb.step();
            StressMessage* slot = nullptr;
            while ((slot = queue->claim()) == nullptr) perturb.blocked();
            perturb.step(); // claimed, not yet written
            slot->seq = i;
            for (unsigned k = 0; k < 6; ++k) slot->payload[k] = stressWord(i, k);
            slot->checksum = stressChecksum(*slot);
            perturb.step(); // written, not yet published: the window edge 1 must cover
            queue->publish();
        }
        if constexpr (requires { queue->flush(); }) queue->flush(); // batched queues hold back the last few
    });

    SchedulePerturber perturb(schedule, seed ^ 0xc2b2ae35ULL);
    for (uint64_t expected = 0; expected < count; ++expected) {
        perturb.step();
        const StressMessage* slot = nullptr;
        while ((slot = queue->peek()) == nullptr) perturb.blocked();
        perturb.step(); // visible, not yet read
        const StressMessage message = *slot; // copy out before consume(): the producer may reuse the slot after
        perturb.step(); // read, not yet released: the window edge 2 must cover
        queue->consume();

        ++result.received;
        if (message.seq != expected) ++result.badSequence;
        for (unsigned k = 0; k < 6; ++k) {
            if (message.payload[k] != stressWord(expected, k)) {
                ++result.badPayload;
                break;
            }
        }
        if (message.checksum != stressChecksum(message)) ++result.badChecksum;
    }

    producer.join();
    return result;
}
