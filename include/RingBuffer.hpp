#pragma once

#include <cstddef>
#include <vector>
#include <atomic>
#include <cassert>
#include <new>

// Cache line size is typically 64 bytes
constexpr size_t CACHE_LINE_SIZE = 64;

// Single-producer / single-consumer queue.
//
// Only the producer writes `head` and only the consumer writes `tail`. A slot belongs to
// the producer from claim() to publish(), then to the consumer from peek() to consume().
// Two release/acquire pairs hand each slot across (docs/memory_ordering.md):
//   edge 1: publish() release -> peek() acquire   the slot's contents are visible to the consumer
//   edge 2: consume() release -> claim() acquire  the consumer has finished reading the slot
//                                                 before the producer writes it again
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
        // Relaxed: the producer is the only writer of head, so it always reads its own last value.
        size_t h = head.load(std::memory_order_relaxed);
        // Acquire, pairs with consume() (edge 2): once we see the consumer's new tail, its reads
        // of the freed slot happen before the writes we are about to make to it.
        size_t t = tail.load(std::memory_order_acquire);

        if (h - t >= Size) {
            return nullptr; // Full
        }

        return &buffer[h & (Size - 1)];
    }

    void publish() {
        // Release, pairs with peek() (edge 1): every write to the claimed slot happens before
        // any consumer load that sees this new head.
        head.fetch_add(1, std::memory_order_release);
    }

    // Consumer: Read from buffer
    // Returns pointer to slot if available, nullptr if empty
    T* peek() {
        // Relaxed: the consumer is the only writer of tail.
        size_t t = tail.load(std::memory_order_relaxed);
        // Acquire, pairs with publish() (edge 1): seeing the new head makes the producer's
        // writes to that slot visible to our reads.
        size_t h = head.load(std::memory_order_acquire);

        if (t == h) {
            return nullptr; // Empty
        }

        return &buffer[t & (Size - 1)];
    }

    void consume() {
        // Release, pairs with claim() (edge 2): our reads of this slot happen before the producer,
        // having seen the new tail, overwrites it.
        tail.fetch_add(1, std::memory_order_release);
    }
};
