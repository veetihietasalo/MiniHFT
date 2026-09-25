#pragma once

#include <cstddef>
#include <vector>
#include <atomic>
#include <cassert>
#include <new>

// Cache line size is typically 64 bytes
constexpr size_t CACHE_LINE_SIZE = 64;

template <typename T, size_t Size>
class RingBuffer {
    static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");

private:
    // Padding to prevent false sharing between head and tail
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head{0};
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail{0};

    // The buffer itself
    alignas(CACHE_LINE_SIZE) T buffer[Size];

public:
    RingBuffer() = default;

    // Producer: Write to buffer
    // Returns pointer to slot if successful, nullptr if full
    T* claim() {
        size_t h = head.load(std::memory_order_relaxed);
        size_t t = tail.load(std::memory_order_acquire);

        if (h - t >= Size) {
            return nullptr; // Full
        }

        return &buffer[h & (Size - 1)];
    }

    void publish() {
        head.fetch_add(1, std::memory_order_release);
    }

    // Consumer: Read from buffer
    // Returns pointer to slot if available, nullptr if empty
    T* peek() {
        size_t t = tail.load(std::memory_order_relaxed);
        size_t h = head.load(std::memory_order_acquire);

        if (t == h) {
            return nullptr; // Empty
        }

        return &buffer[t & (Size - 1)];
    }

    void consume() {
        tail.fetch_add(1, std::memory_order_release);
    }
};
