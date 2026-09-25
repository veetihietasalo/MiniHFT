#include <gtest/gtest.h>

#include <thread>

#include "ThreadUtils.hpp"

// Each case runs on a fresh thread so pinning never leaks into the test runner's main thread.

TEST(ThreadUtils, PinThreadMovesCallerToRequestedCore) {
    const int cores = static_cast<int>(std::thread::hardware_concurrency());
    ASSERT_GT(cores, 0);

    for (int core : {0, cores - 1}) {
        std::thread worker([core] {
            ASSERT_TRUE(ThreadUtils::pinThread(core)) << "core " << core;
            std::this_thread::yield();
            EXPECT_EQ(ThreadUtils::currentCore(), core);
        });
        worker.join();
    }
}

TEST(ThreadUtils, PinThreadRejectsCoreThatDoesNotExist) {
    const int cores = static_cast<int>(std::thread::hardware_concurrency());
    ASSERT_GT(cores, 0);

    std::thread worker([cores] {
        EXPECT_FALSE(ThreadUtils::pinThread(cores)); // valid ids are 0 .. cores-1
    });
    worker.join();
}
