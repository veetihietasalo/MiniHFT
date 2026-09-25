#pragma once

// Two-thread stress test, shared by the unit tests and the relaxed-publish demo.
//
// Each message fills one 64-byte cache line: a sequence number, six payload words derived
// from it, and a checksum. A consumer that reads a slot before the producer's writes are
// visible sees a wrong sequence number, stale payload words or a checksum that doesn't match.

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

template <typename Queue>
StressResult runSpscStress(uint64_t count) {
    auto queue = std::make_unique<Queue>();
    StressResult result;

    std::thread producer([&] {
        for (uint64_t i = 0; i < count; ++i) {
            StressMessage* slot = nullptr;
            while ((slot = queue->claim()) == nullptr) std::this_thread::yield();
            slot->seq = i;
            for (unsigned k = 0; k < 6; ++k) slot->payload[k] = stressWord(i, k);
            slot->checksum = stressChecksum(*slot);
            queue->publish();
        }
        if constexpr (requires { queue->flush(); }) queue->flush(); // batched queues hold back the last few
    });

    for (uint64_t expected = 0; expected < count; ++expected) {
        const StressMessage* slot = nullptr;
        while ((slot = queue->peek()) == nullptr) std::this_thread::yield();
        const StressMessage message = *slot; // copy out before consume(): the producer may reuse the slot after
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
