#pragma once

// DELIBERATELY BROKEN copy of RingBuffer, for the memory-model exercise (roadmap W02).
//
// publish() uses memory_order_relaxed instead of release. The consumer's acquire load of
// head then has no release to synchronize with, so its reads of a slot race with the
// producer's writes to it. Test-only: never use this outside tests/.

#include <atomic>
#include <cstddef>

#include "RingBuffer.hpp" // CACHE_LINE_SIZE

template <typename T, size_t Size>
class RelaxedPublishRingBuffer {
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
        head.fetch_add(1, std::memory_order_relaxed); // THE BUG: RingBuffer uses release here
    }

    T* peek() {
        size_t t = tail.load(std::memory_order_relaxed);
        size_t h = head.load(std::memory_order_acquire);
        if (t == h) return nullptr;
        return &buffer[t & (Size - 1)];
    }

    void consume() {
        tail.fetch_add(1, std::memory_order_release);
    }
};
