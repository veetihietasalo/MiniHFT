#pragma once

// DELIBERATELY BROKEN copy of RingBuffer: the mirror image of RelaxedPublishRingBuffer.
//
// consume() uses memory_order_relaxed instead of release. The producer's acquire load of tail
// then has no release to synchronize with, so when it reuses a slot, its writes race with the
// consumer's earlier reads of that slot (edge 2 in docs/memory_ordering.md). The race needs the
// producer to come back round to a slot, so it shows only once more messages than slots have
// gone through: a tiny queue makes that immediate. Test-only: never use this outside tests/.

#include <atomic>
#include <cstddef>

#include "RingBuffer.hpp" // CACHE_LINE_SIZE

template <typename T, size_t Size>
class RelaxedConsumeRingBuffer {
    static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");

    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head{0};
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail{0};
    alignas(CACHE_LINE_SIZE) T buffer[Size];

public:
    T* claim() {
        size_t h = head.load(std::memory_order_relaxed);
        size_t t = tail.load(std::memory_order_acquire);
        if (h - t >= Size) return nullptr;
        return &buffer[h & (Size - 1)];
    }

    void publish() {
        head.fetch_add(1, std::memory_order_release);
    }

    T* peek() {
        size_t t = tail.load(std::memory_order_relaxed);
        size_t h = head.load(std::memory_order_acquire);
        if (t == h) return nullptr;
        return &buffer[t & (Size - 1)];
    }

    void consume() {
        tail.fetch_add(1, std::memory_order_relaxed); // THE BUG: RingBuffer uses release here
    }
};
