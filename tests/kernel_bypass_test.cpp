#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <thread>

#include "KernelBypass.hpp"

namespace {
constexpr size_t kRingSize = KernelBypass::RING_SIZE;
constexpr uint32_t kStressPackets = 200'000; // about 195 laps of the ring
} // namespace

TEST(KernelBypass, PollReturnsPacketsInArrivalOrder) {
    KernelBypass nic;
    uint32_t packet = 0;
    EXPECT_FALSE(nic.poll(packet));
    for (uint32_t i = 0; i < 3; ++i) ASSERT_TRUE(nic.nicReceive(100 + i, i));
    for (uint32_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(nic.poll(packet));
        EXPECT_EQ(packet, 100 + i);
    }
    EXPECT_FALSE(nic.poll(packet));
}

// A real NIC drops a packet when the driver has no free descriptor. The simulation used to
// overwrite the unread packet instead, so packet 0 was lost.
TEST(KernelBypass, FullRingRefusesPacketsUntilTheCpuPolls) {
    KernelBypass nic;
    for (size_t i = 0; i < kRingSize; ++i) ASSERT_TRUE(nic.nicReceive(static_cast<uint32_t>(i), i));
    EXPECT_FALSE(nic.nicReceive(9999, kRingSize)); // slot 0 again, still holding packet 0

    uint32_t packet = 0;
    ASSERT_TRUE(nic.poll(packet));
    EXPECT_EQ(packet, 0u);
    ASSERT_TRUE(nic.nicReceive(static_cast<uint32_t>(kRingSize), kRingSize)); // slot 0 is free again
    for (size_t i = 1; i <= kRingSize; ++i) {
        ASSERT_TRUE(nic.poll(packet));
        EXPECT_EQ(packet, static_cast<uint32_t>(i));
    }
    EXPECT_FALSE(nic.poll(packet));
}

// The NIC and the CPU on two threads, many laps of the ring. Run it under the clang-tsan
// preset: ThreadSanitizer reports a data race if either handoff of a descriptor stops
// synchronizing, in either direction.
TEST(KernelBypass, TwoThreadsDeliverEveryPacketInOrder) {
    KernelBypass nic;
    std::thread wire([&nic] {
        for (uint32_t i = 0; i < kStressPackets; ++i) {
            while (!nic.nicReceive(i, i)) std::this_thread::yield(); // ring full: wait for the CPU
        }
    });

    uint32_t outOfOrder = 0;
    for (uint32_t expected = 0; expected < kStressPackets; ++expected) {
        uint32_t packet = 0;
        while (!nic.poll(packet)) std::this_thread::yield();
        if (packet != expected) ++outOfOrder;
    }
    wire.join();
    EXPECT_EQ(outOfOrder, 0u);
}
