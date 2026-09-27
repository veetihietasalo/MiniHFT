#pragma once

// Counts heap allocations. allocation_counter.cpp replaces the global operator new and delete
// for the whole test binary; counting is per thread and only on inside countAllocations().

#include <cstddef>

namespace test_alloc {
    void begin();     // start counting on this thread, from zero
    std::size_t end(); // stop counting; returns the number of allocations since begin()
}

// How many times `fn` allocated from the heap on the calling thread.
template <typename Fn>
std::size_t countAllocations(Fn&& fn) {
    test_alloc::begin();
    fn();
    return test_alloc::end();
}
