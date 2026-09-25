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
//
// Each side keeps a private copy of the other side's index and re-reads the shared one only
// when the queue looks full (producer) or empty (consumer). In steady state neither thread
// reads the other's cache line on every call. Measurements: docs/ring_buffer_v2.md.
template <typename T, size_t Size>
class RingBuffer {
    static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");

private:
    // Written only by the producer; kept on its own cache line.
    struct alignas(CACHE_LINE_SIZE) ProducerSide {
        std::atomic<size_t> head{0}; // next slot to publish; read by the consumer
        size_t cachedTail = 0;       // last tail value the producer saw
    };
    // Written only by the consumer; kept on its own cache line.
    struct alignas(CACHE_LINE_SIZE) ConsumerSide {
        std::atomic<size_t> tail{0}; // next slot to consume; read by the producer
        size_t cachedHead = 0;       // last head value the consumer saw
    };

    ProducerSide producer_;
    ConsumerSide consumer_;

    // The buffer itself
    alignas(CACHE_LINE_SIZE) T buffer[Size];

public:
    RingBuffer() = default;

    // Producer: Write to buffer
    // Returns pointer to slot if successful, nullptr if full
    T* claim() {
        // Relaxed: the producer is the only writer of head, so it always reads its own last value.
        const size_t h = producer_.head.load(std::memory_order_relaxed);
        if (h - producer_.cachedTail >= Size) {
            // Looks full by our cached view, so re-read the real tail.
            // Acquire, pairs with consume() (edge 2): once we see the consumer's new tail, its
            // reads of the freed slots happen before the writes we are about to make to them.
            producer_.cachedTail = consumer_.tail.load(std::memory_order_acquire);
            if (h - producer_.cachedTail >= Size) {
                return nullptr; // Full
            }
        }
        return &buffer[h & (Size - 1)];
    }

    void publish() {
        // Release, pairs with peek() (edge 1): every write to the claimed slot happens before
        // any consumer load that sees this new head. head has a single writer, so a plain
        // store is enough; a locked read-modify-write (fetch_add) would only add cost.
        producer_.head.store(producer_.head.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    // Consumer: Read from buffer
    // Returns pointer to slot if available, nullptr if empty
    T* peek() {
        // Relaxed: the consumer is the only writer of tail.
        const size_t t = consumer_.tail.load(std::memory_order_relaxed);
        if (t == consumer_.cachedHead) {
            // Looks empty by our cached view, so re-read the real head.
            // Acquire, pairs with publish() (edge 1): seeing the new head makes the producer's
            // writes to every slot before it visible to our reads.
            consumer_.cachedHead = producer_.head.load(std::memory_order_acquire);
            if (t == consumer_.cachedHead) {
                return nullptr; // Empty
            }
        }
        return &buffer[t & (Size - 1)];
    }

    void consume() {
        // Release, pairs with claim() (edge 2): our reads of this slot happen before the producer,
        // having seen the new tail, overwrites it.
        consumer_.tail.store(consumer_.tail.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }
};
