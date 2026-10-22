#pragma once

// DELIBERATELY BROKEN copies of KernelBypass, for measuring what the stress tests catch.
// Test-only: never use this outside tests/.
//
// Each breaks one of the two handoffs of a descriptor (include/KernelBypass.hpp):
//   ReceiveRelaxed  nicReceive() stores `ready` relaxed: the CPU may see the flag before the
//                   packet data, and read a stale packet
//   PollRelaxed     poll() clears `ready` relaxed: the NIC may see the descriptor free before the
//                   CPU's read of the data has completed, and overwrite it mid-read
// Unlike RingBuffer, whose index lives on its own cache line, a descriptor's data and flag share
// 8 bytes. Whether a real CPU reorders stores that close together is what these measure.

#include <atomic>
#include <cstddef>
#include <cstdint>

enum class KernelBypassBug : uint8_t { ReceiveRelaxed, PollRelaxed };

template <size_t RingSize, KernelBypassBug Bug>
class RelaxedKernelBypass {
    static_assert(RingSize > 0, "RingSize must be at least 1");

    static constexpr std::memory_order kReceiveStore =
        Bug == KernelBypassBug::ReceiveRelaxed ? std::memory_order_relaxed : std::memory_order_release;
    static constexpr std::memory_order kPollStore =
        Bug == KernelBypassBug::PollRelaxed ? std::memory_order_relaxed : std::memory_order_release;

    struct Descriptor {
        uint32_t data = 0;
        std::atomic<bool> ready{false};
    };

    Descriptor rxRing[RingSize];
    size_t head = 0;

public:
    static constexpr size_t RING_SIZE = RingSize;

    [[nodiscard]] bool nicReceive(uint32_t packetData, size_t slot) {
        Descriptor& d = rxRing[slot % RingSize];
        if (d.ready.load(std::memory_order_acquire)) return false;
        d.data = packetData;
        d.ready.store(true, kReceiveStore); // THE BUG when ReceiveRelaxed
        return true;
    }

    [[nodiscard]] bool poll(uint32_t& outData) {
        Descriptor& d = rxRing[head];
        if (!d.ready.load(std::memory_order_acquire)) return false;
        outData = d.data;
        d.ready.store(false, kPollStore); // THE BUG when PollRelaxed
        head = (head + 1) % RingSize;
        return true;
    }
};
