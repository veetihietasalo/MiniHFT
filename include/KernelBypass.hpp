#pragma once

#include <vector>
#include <cstdint>
#include <atomic>
#include <thread>
#include <iostream>

// Simulating a Network Interface Card (NIC) Ring Buffer
// This memory is "mapped" into user space, bypassing the OS kernel.
class KernelBypass {
private:
    static constexpr size_t RING_SIZE = 1024;
    struct Descriptor {
        volatile uint32_t data;
        volatile bool ready; // HW sets this to true when packet arrives
    };

    Descriptor rxRing[RING_SIZE];
    size_t head = 0; // CPU reads from here

public:
    KernelBypass() {
        for (auto& d : rxRing) d.ready = false;
    }

    // Simulates the NIC hardware receiving a packet
    // This would happen asynchronously in real hardware
    void nicReceive(uint32_t packetData, size_t slot) {
        rxRing[slot % RING_SIZE].data = packetData;
        std::atomic_thread_fence(std::memory_order_release);
        rxRing[slot % RING_SIZE].ready = true; // Mark as ready
    }

    // Poll Mode Driver (PMD)
    // CPU spins here waiting for the "ready" bit. No interrupts!
    bool poll(uint32_t& outData) {
        if (rxRing[head].ready) {
            std::atomic_thread_fence(std::memory_order_acquire);
            outData = rxRing[head].data;
            rxRing[head].ready = false; // Clear for reuse
            head = (head + 1) % RING_SIZE;
            return true;
        }
        return false; // Nothing received
    }
};
