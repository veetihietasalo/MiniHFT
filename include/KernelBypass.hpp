#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// Simulating a Network Interface Card (NIC) Ring Buffer
// This memory is "mapped" into user space, bypassing the OS kernel.
//
// Each descriptor's `ready` flag hands it from one side to the other, as RingBuffer's indices
// hand over its slots (docs/memory_ordering.md):
//   nicReceive() release -> poll() acquire         the packet data is visible to the CPU
//   poll() release       -> nicReceive() acquire   the CPU has read the data before the NIC
//                                                  writes that descriptor again
class KernelBypass {
public:
    static constexpr size_t RING_SIZE = 1024;

private:
    struct Descriptor {
        uint32_t data = 0;
        std::atomic<bool> ready{false}; // HW sets this to true when packet arrives; the CPU clears it
    };

    Descriptor rxRing[RING_SIZE];
    size_t head = 0; // CPU reads from here

public:
    // Simulates the NIC hardware receiving a packet
    // This would happen asynchronously in real hardware
    // Returns false if the CPU hasn't polled the slot's previous packet yet: the ring is full,
    // and a real NIC drops the packet.
    [[nodiscard]] bool nicReceive(uint32_t packetData, size_t slot) {
        Descriptor& d = rxRing[slot % RING_SIZE];
        if (d.ready.load(std::memory_order_acquire)) return false; // pairs with poll()'s release
        d.data = packetData;
        d.ready.store(true, std::memory_order_release); // pairs with poll()'s acquire
        return true;
    }

    // Poll Mode Driver (PMD)
    // CPU spins here waiting for the "ready" bit. No interrupts!
    [[nodiscard]] bool poll(uint32_t& outData) {
        Descriptor& d = rxRing[head];
        if (!d.ready.load(std::memory_order_acquire)) return false; // Nothing received
        outData = d.data;
        d.ready.store(false, std::memory_order_release); // Clear for reuse: hand it back to the NIC
        head = (head + 1) % RING_SIZE;
        return true;
    }
};
