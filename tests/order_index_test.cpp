#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <random>
#include <unordered_map>

#include "OrderIndex.hpp"

TEST(OrderIndex, FindInsertOverwriteErase) {
    OrderIndex<int> index;
    EXPECT_EQ(index.find(42), nullptr);
    index.insert(42, 1);
    ASSERT_NE(index.find(42), nullptr);
    EXPECT_EQ(*index.find(42), 1);
    index.insert(42, 2);
    EXPECT_EQ(*index.find(42), 2);
    EXPECT_EQ(index.size(), 1u);
    EXPECT_TRUE(index.erase(42));
    EXPECT_FALSE(index.erase(42));
    EXPECT_EQ(index.find(42), nullptr);
    EXPECT_EQ(index.size(), 0u);
}

// Random inserts and erases against std::unordered_map. A small initial size forces many
// rehashes; erasing inside long probe clusters checks the backward-shift deletion.
TEST(OrderIndex, MatchesUnorderedMapUnderRandomChurn) {
    OrderIndex<uint64_t> index(4);
    std::unordered_map<uint64_t, uint64_t> reference;
    std::mt19937_64 rng(1234);
    uint64_t nextRef = 1;

    for (int step = 0; step < 200'000; ++step) {
        const auto op = rng() % 10;
        if (op < 5 || reference.empty()) { // insert a new ref (sequential, like ITCH)
            const uint64_t ref = nextRef++;
            index.insert(ref, ref * 3);
            reference[ref] = ref * 3;
        } else if (op < 9) { // erase something that exists
            auto it = reference.begin();
            std::advance(it, static_cast<long>(rng() % std::min<size_t>(reference.size(), 16)));
            ASSERT_TRUE(index.erase(it->first));
            reference.erase(it);
        } else { // erase something that doesn't
            ASSERT_FALSE(index.erase(nextRef + 1'000'000));
        }
        ASSERT_EQ(index.size(), reference.size());
    }
    for (const auto& [key, value] : reference) {
        const uint64_t* found = index.find(key);
        ASSERT_NE(found, nullptr) << "key " << key;
        EXPECT_EQ(*found, value);
    }
    for (uint64_t ref = 1; ref < nextRef; ++ref) {
        EXPECT_EQ(index.find(ref) != nullptr, reference.count(ref) == 1) << "ref " << ref;
    }
}
