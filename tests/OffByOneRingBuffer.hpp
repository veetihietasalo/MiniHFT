#pragma once

// DELIBERATELY BROKEN copy of RingBuffer: every memory order is right, but claim()'s full check
// is off by one (`>` instead of `>=`), so the producer can claim the slot the consumer is about
// to read, or is reading.
//
// Unlike the relaxed-publish and relaxed-consume queues, this race happens only in some
// interleavings: the queue has to be completely full at the moment the producer claims.
// ThreadSanitizer can only report it in a run where that happens, which makes it the case for
// measuring what schedule perturbation (tests/SpscStress.hpp) adds. Test-only: never use this
// outside tests/.

#include <atomic>
#include <cstddef>

#include "RingBuffer.hpp" // CACHE_LINE_SIZE

template <typename T, size_t Size>
class OffByOneRingBuffer {
    static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");

    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head{0};
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail{0};
    alignas(CACHE_LINE_SIZE) T buffer[Size];

public:
    T* claim() {
        size_t h = head.load(std::memory_order_relaxed);
        size_t t = tail.load(std::memory_order_acquire);
        if (h - t > Size) return nullptr; // THE BUG: RingBuffer refuses at h - t >= Size
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
        tail.fetch_add(1, std::memory_order_release);
    }
};
