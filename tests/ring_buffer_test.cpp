#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <thread>

#include "RingBuffer.hpp"
#include "SpscStress.hpp"

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

// Two threads, one cache-line-sized message each. Run it under the clang-tsan preset:
// ThreadSanitizer reports a data race if either release/acquire pair stops synchronizing.
TEST(RingBuffer, SpscStressDeliversEveryMessageIntactAndInOrder) {
    constexpr uint64_t kCount = 300'000;
    const StressResult r = runSpscStress<RingBuffer<StressMessage, 1024>>(kCount);
    EXPECT_EQ(r.received, kCount);
    EXPECT_EQ(r.badSequence, 0u);
    EXPECT_EQ(r.badPayload, 0u);
    EXPECT_EQ(r.badChecksum, 0u);
}

// An 8-slot queue is full or empty most of the time, so every slot changes hands constantly.
// That leans on edge 2 (consume() release -> claim() acquire): the producer must never
// overwrite a slot the consumer is still copying.
TEST(RingBuffer, SpscStressWithTinyQueueNeverOverwritesUnreadSlots) {
    constexpr uint64_t kCount = 100'000;
    const StressResult r = runSpscStress<RingBuffer<StressMessage, 8>>(kCount);
    EXPECT_EQ(r.received, kCount);
    EXPECT_TRUE(r.clean()) << r.badSequence << " bad sequence, " << r.badPayload << " bad payload, "
                           << r.badChecksum << " bad checksum";
}
