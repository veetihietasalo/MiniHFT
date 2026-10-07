#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <random>
#include <unordered_map>
#include <utility>

#include "OrderIndex.hpp"

TEST(OrderIndex, FindInsertErase) {
    OrderIndex<int> index;
    EXPECT_EQ(index.find(42), nullptr);
    EXPECT_TRUE(index.tryInsert(42, 1));
    ASSERT_NE(index.find(42), nullptr);
    EXPECT_EQ(*index.find(42), 1);
    EXPECT_EQ(index.size(), 1u);
    EXPECT_TRUE(index.erase(42));
    EXPECT_FALSE(index.erase(42));
    EXPECT_EQ(index.find(42), nullptr);
    EXPECT_EQ(index.size(), 0u);
}

// An existing entry is never overwritten: a second insert of a live key is refused.
TEST(OrderIndex, DuplicateKeyIsRefusedAndKeepsTheFirstValue) {
    OrderIndex<int> index;
    EXPECT_TRUE(index.tryInsert(42, 1));
    EXPECT_FALSE(index.tryInsert(42, 2));
    EXPECT_EQ(*index.find(42), 1);
    EXPECT_EQ(index.size(), 1u);
}

// kEmpty marks unused slots, so it can't be a key. It must be refused, not stored.
TEST(OrderIndex, ReservedKeyIsRefusedAndNeverFound) {
    OrderIndex<int> index;
    EXPECT_FALSE(index.tryInsert(OrderIndex<int>::kEmpty, 1));
    EXPECT_EQ(index.size(), 0u);
    EXPECT_EQ(index.find(OrderIndex<int>::kEmpty), nullptr);
    EXPECT_FALSE(index.erase(OrderIndex<int>::kEmpty));
    EXPECT_EQ(index.size(), 0u);
}

// A moved-from index is empty and can be used again. The implicit moves took the slots but kept
// the old size, mask and shift, so the next call on the source indexed an empty table.
TEST(OrderIndex, MovedFromIndexIsEmptyAndUsable) {
    OrderIndex<int> source;
    ASSERT_TRUE(source.tryInsert(1, 10));
    OrderIndex<int> moved(std::move(source));
    ASSERT_NE(moved.find(1), nullptr);
    EXPECT_EQ(*moved.find(1), 10);

    OrderIndex<int> assigned;
    ASSERT_TRUE(assigned.tryInsert(3, 30));
    assigned = std::move(moved);
    ASSERT_NE(assigned.find(1), nullptr);
    EXPECT_EQ(assigned.find(3), nullptr);

    // Using the moved-from objects is what this test is about.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    for (OrderIndex<int>* from : {&source, &moved}) {
        EXPECT_EQ(from->size(), 0u);
        EXPECT_EQ(from->find(1), nullptr);
        EXPECT_FALSE(from->erase(1));
        EXPECT_TRUE(from->tryInsert(2, 20));
        ASSERT_NE(from->find(2), nullptr);
        EXPECT_EQ(*from->find(2), 20);
        EXPECT_EQ(from->size(), 1u);
    }
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
            ASSERT_TRUE(index.tryInsert(ref, ref * 3));
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
