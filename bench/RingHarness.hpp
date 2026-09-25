#pragma once

// Two pinned threads sending messages through any queue with claim()/publish()/peek()/consume().
// Shared by ring_latency and ring_study.
//
// With recordLatency, every message carries TSC stamps and the consumer records
// send->receive and intended->receive into histograms. Without it (throughput runs) the
// producer only writes a sequence number and the consumer only times the whole run, so the
// timestamping itself doesn't limit throughput. A queue with flush() (batched publishing)
// is flushed after the last message.

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "LatencyHistogram.hpp"
#include "ThreadUtils.hpp"
#include "Tsc.hpp"

struct RingMessage {
    uint64_t seq;
    uint64_t intendedTsc;
    uint64_t sentTsc;
};

struct RingRunConfig {
    int producerCore = 2;
    int consumerCore = 3;
    uint64_t messages = 1'000'000;
    uint64_t warmup = 50'000;
    uint64_t intervalTicks = 0; // 0 = back-to-back
    uint64_t producerWorkTicks = 0; // busy work per message before claim() (e.g. parsing a packet)
    uint64_t consumerWorkTicks = 0; // busy work per message after consume() (e.g. running a strategy)
    bool highPriority = false;
    bool recordLatency = true;
};

struct RingRunResult {
    LatencyHistogram sendToReceive;
    LatencyHistogram intendedToReceive;
    uint64_t firstReceiveTsc = 0; // first measured message (after warm-up)
    uint64_t lastReceiveTsc = 0;  // last message
    uint64_t outOfOrder = 0;
    uint64_t negativeDeltas = 0;  // receive stamped before send: would mean the TSCs are not in sync
    bool producerPinned = false;
    bool consumerPinned = false;

    double nsPerMessage(uint64_t messages, double ticksPerNs) const {
        return static_cast<double>(lastReceiveTsc - firstReceiveTsc) / ticksPerNs / static_cast<double>(messages);
    }
};

namespace ring_harness_detail {
    inline uint64_t elapsed(uint64_t from, uint64_t to, uint64_t& negativeDeltas) {
        if (to >= from) return to - from;
        ++negativeDeltas;
        return 0;
    }

    inline void busyWork(uint64_t ticks) {
        if (ticks == 0) return;
        const uint64_t until = Tsc::read() + ticks;
        while (Tsc::read() < until) {}
    }
}

template <typename Queue>
std::unique_ptr<RingRunResult> runRing(const RingRunConfig& cfg, double ticksPerNs) {
    auto queue = std::make_unique<Queue>();
    auto r = std::make_unique<RingRunResult>();
    std::atomic<bool> consumerReady{false};
    const uint64_t total = cfg.warmup + cfg.messages;
    const bool paced = cfg.intervalTicks > 0;

    std::thread consumer([&] {
        r->consumerPinned = ThreadUtils::pinThread(cfg.consumerCore);
        if (cfg.highPriority) ThreadUtils::setHighPriority();
        consumerReady.store(true, std::memory_order_release);

        for (uint64_t i = 0; i < total; ++i) {
            RingMessage* msg = nullptr;
            while ((msg = queue->peek()) == nullptr) {} // pure spin: lowest wake-up latency
            if (cfg.recordLatency) {
                const uint64_t now = Tsc::readOrdered();
                if (i >= cfg.warmup) {
                    r->sendToReceive.record(ring_harness_detail::elapsed(msg->sentTsc, now, r->negativeDeltas));
                    if (paced) r->intendedToReceive.record(ring_harness_detail::elapsed(msg->intendedTsc, now, r->negativeDeltas));
                }
            }
            if (msg->seq != i) ++r->outOfOrder;
            if (i == cfg.warmup) r->firstReceiveTsc = Tsc::readOrdered();
            queue->consume();
            ring_harness_detail::busyWork(cfg.consumerWorkTicks);
        }
        r->lastReceiveTsc = Tsc::readOrdered();
    });

    std::thread producer([&] {
        r->producerPinned = ThreadUtils::pinThread(cfg.producerCore);
        if (cfg.highPriority) ThreadUtils::setHighPriority();
        while (!consumerReady.load(std::memory_order_acquire)) {}

        const uint64_t start = Tsc::read() + static_cast<uint64_t>(1e6 * ticksPerNs); // first send 1 ms from now
        for (uint64_t i = 0; i < total; ++i) {
            const uint64_t intended = start + i * cfg.intervalTicks;
            if (paced) {
                while (Tsc::read() < intended) Tsc::cpuRelax();
            }
            ring_harness_detail::busyWork(cfg.producerWorkTicks);
            RingMessage* msg = nullptr;
            while ((msg = queue->claim()) == nullptr) {}
            msg->seq = i;
            if (cfg.recordLatency) {
                msg->intendedTsc = intended;
                msg->sentTsc = Tsc::read();
            }
            queue->publish();
        }
        if constexpr (requires { queue->flush(); }) queue->flush();
    });

    producer.join();
    consumer.join();
    return r;
}
