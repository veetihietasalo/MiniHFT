#include <gtest/gtest.h>

#include <cstdint>

#include "../bench/RingVariants.hpp"
#include "SpscStress.hpp"

// Every queue variant in the W03 study must be correct before its numbers mean anything.
// Under the clang-tsan preset these also check that none of them has a data race.

template <typename Queue>
class RingVariantTest : public ::testing::Test {};

using Variants = ::testing::Types<
    RingV0FetchAdd<StressMessage, 1024>,
    RingV1Store<StressMessage, 1024>,
    RingV2<StressMessage, 1024>,
    RingV2<StressMessage, 1024, 1, false>, // false sharing: slower, still correct
    RingV2<StressMessage, 1024, 4>,
    RingV2<StressMessage, 1024, 64>,
    RingV2<StressMessage, 8, 8>>;          // batch == size: the tightest legal batching
TYPED_TEST_SUITE(RingVariantTest, Variants);

TYPED_TEST(RingVariantTest, DeliversEveryMessageIntactAndInOrder) {
    constexpr uint64_t kCount = 100'003; // not a multiple of any batch size: flush() must publish the tail end
    const StressResult r = runSpscStress<TypeParam>(kCount);
    EXPECT_EQ(r.received, kCount);
    EXPECT_TRUE(r.clean()) << r.badSequence << " bad sequence, " << r.badPayload << " bad payload, "
                           << r.badChecksum << " bad checksum";
}
