#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <thread>

#include "RingBuffer.hpp"

namespace {

constexpr size_t kSize = 8;
using SmallRing = RingBuffer<uint64_t, kSize>;

bool tryPush(SmallRing& rb, uint64_t value) {
    uint64_t* slot = rb.claim();
    if (slot == nullptr) return false;
    *slot = value;
    rb.publish();
    return true;
}

bool tryPop(SmallRing& rb, uint64_t& out) {
    uint64_t* slot = rb.peek();
    if (slot == nullptr) return false;
    out = *slot;
    rb.consume();
    return true;
}

} // namespace

TEST(RingBuffer, StartsEmpty) {
    SmallRing rb;
    EXPECT_EQ(rb.peek(), nullptr);
    EXPECT_NE(rb.claim(), nullptr);
}

TEST(RingBuffer, PreservesFifoOrder) {
    SmallRing rb;
    for (uint64_t i = 0; i < kSize; ++i) ASSERT_TRUE(tryPush(rb, i));
    for (uint64_t i = 0; i < kSize; ++i) {
        uint64_t v = 0;
        ASSERT_TRUE(tryPop(rb, v));
        EXPECT_EQ(v, i);
    }
    EXPECT_EQ(rb.peek(), nullptr);
}

TEST(RingBuffer, RejectsClaimWhenFullUntilConsumed) {
    SmallRing rb;
    for (uint64_t i = 0; i < kSize; ++i) ASSERT_TRUE(tryPush(rb, i));
    EXPECT_EQ(rb.claim(), nullptr);

    uint64_t v = 0;
    ASSERT_TRUE(tryPop(rb, v));
    EXPECT_NE(rb.claim(), nullptr);
}

TEST(RingBuffer, WrapsAroundManyTimes) {
    SmallRing rb;
    for (uint64_t i = 0; i < kSize * 100; ++i) {
        ASSERT_TRUE(tryPush(rb, i));
        uint64_t v = 0;
        ASSERT_TRUE(tryPop(rb, v));
        ASSERT_EQ(v, i);
    }
}

// One producer thread, one consumer thread. Run it under the clang-tsan preset:
// ThreadSanitizer reports a data race if publish()/peek() stop synchronizing.
TEST(RingBuffer, SpscDeliversEverySequenceNumberInOrder) {
    constexpr uint64_t kCount = 200'000;
    auto rb = std::make_unique<RingBuffer<uint64_t, 1024>>(); // over-aligned and > 8 KB: keep it off the stack

    std::thread producer([&] {
        for (uint64_t i = 0; i < kCount; ++i) {
            uint64_t* slot = nullptr;
            while ((slot = rb->claim()) == nullptr) std::this_thread::yield();
            *slot = i;
            rb->publish();
        }
    });

    uint64_t expected = 0;
    uint64_t firstMismatch = kCount; // kCount means "none"
    while (expected < kCount) {
        uint64_t* slot = rb->peek();
        if (slot == nullptr) {
            std::this_thread::yield();
            continue;
        }
        if (*slot != expected && firstMismatch == kCount) firstMismatch = expected;
        rb->consume();
        ++expected;
    }
    producer.join();

    EXPECT_EQ(firstMismatch, kCount) << "first out-of-order value at sequence " << firstMismatch;
}
