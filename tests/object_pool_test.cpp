#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <vector>

#include "ObjectPool.hpp"

namespace {
struct Item {
    uint64_t a = 0;
    uint64_t b = 0;
};
} // namespace

// Growing past the first block used to try to re-point a reference to the new block, which
// can't be done in C++. That path didn't even compile until the pool was first used for real.
TEST(ObjectPool, GrowsPastOneBlockWithDistinctStableObjects) {
    ObjectPool<Item, 8> pool;
    std::vector<Item*> items;
    for (uint64_t i = 0; i < 100; ++i) { // 13 blocks
        Item* p = pool.acquire();
        p->a = i;
        p->b = ~i;
        items.push_back(p);
    }
    std::set<Item*> distinct(items.begin(), items.end());
    EXPECT_EQ(distinct.size(), items.size());
    for (uint64_t i = 0; i < items.size(); ++i) { // earlier objects were not moved or overwritten
        EXPECT_EQ(items[i]->a, i);
        EXPECT_EQ(items[i]->b, ~i);
    }
}

TEST(ObjectPool, ReusesReleasedObjects) {
    ObjectPool<Item, 8> pool;
    Item* first = pool.acquire();
    pool.release(first);
    Item* again = pool.acquire();
    EXPECT_EQ(again, first);
    EXPECT_EQ(again->a, 0u); // acquire() constructs a fresh object
}
