#pragma once

// RingBuffer design steps for the W03 study (bench/ring_study.cpp). Each variant differs
// from the one before it by exactly one change, so a before/after measurement isolates it.
// All keep the same claim()/publish()/peek()/consume() interface and the same two
// release/acquire edges described in docs/memory_ordering.md.
//
//   RingV0FetchAdd     the original queue: locked fetch_add on every publish/consume,
//                      and every call reads the other thread's index
//   RingV1Store        fetch_add -> store(release): each index has a single writer,
//                      so no locked read-modify-write is needed
//   RingV2<...>        plus cached indices: each side keeps a private copy of the other
//                      side's index and re-reads the shared one only when the queue
//                      looks full (producer) or empty (consumer)
//                      Knobs: SeparateLines=false packs both sides onto one cache line
//                      (false sharing on purpose); Batch>1 publishes head once per Batch
//                      messages (call flush() after the last one)

#include <atomic>
#include <cstddef>

#include "RingBuffer.hpp" // CACHE_LINE_SIZE

template <typename T, size_t Size>
class RingV0FetchAdd {
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
    void publish() { head.fetch_add(1, std::memory_order_release); }
    T* peek() {
        size_t t = tail.load(std::memory_order_relaxed);
        size_t h = head.load(std::memory_order_acquire);
        if (t == h) return nullptr;
        return &buffer[t & (Size - 1)];
    }
    void consume() { tail.fetch_add(1, std::memory_order_release); }
};

template <typename T, size_t Size>
class RingV1Store {
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
    void publish() { head.store(head.load(std::memory_order_relaxed) + 1, std::memory_order_release); }
    T* peek() {
        size_t t = tail.load(std::memory_order_relaxed);
        size_t h = head.load(std::memory_order_acquire);
        if (t == h) return nullptr;
        return &buffer[t & (Size - 1)];
    }
    void consume() { tail.store(tail.load(std::memory_order_relaxed) + 1, std::memory_order_release); }
};

template <typename T, size_t Size, size_t Batch = 1, bool SeparateLines = true>
class RingV2 {
    static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");
    static_assert(Batch >= 1 && Batch <= Size, "Batch must be in [1, Size]");

    static constexpr size_t kSideAlign = SeparateLines ? CACHE_LINE_SIZE : alignof(size_t);

    // Everything the producer writes lives together, and likewise for the consumer.
    struct alignas(kSideAlign) ProducerSide {
        std::atomic<size_t> head{0}; // shared: published write position
        size_t writeIdx = 0;         // private: next slot to fill (runs ahead of head when batching)
        size_t cachedTail = 0;       // private: last tail value read
    };
    struct alignas(kSideAlign) ConsumerSide {
        std::atomic<size_t> tail{0}; // shared: released read position
        size_t cachedHead = 0;       // private: last head value read
    };

    alignas(CACHE_LINE_SIZE) ProducerSide p_;
    ConsumerSide c_; // SeparateLines=false: shares p_'s cache line
    alignas(CACHE_LINE_SIZE) T buffer_[Size];

public:
    T* claim() {
        if (p_.writeIdx - p_.cachedTail >= Size) {
            p_.cachedTail = c_.tail.load(std::memory_order_acquire); // edge 2
            if (p_.writeIdx - p_.cachedTail >= Size) return nullptr;
        }
        return &buffer_[p_.writeIdx & (Size - 1)];
    }

    void publish() {
        ++p_.writeIdx;
        if constexpr (Batch == 1) {
            p_.head.store(p_.writeIdx, std::memory_order_release); // edge 1
        } else if (p_.writeIdx - p_.head.load(std::memory_order_relaxed) >= Batch) {
            p_.head.store(p_.writeIdx, std::memory_order_release);
        }
    }

    // Publish anything still held back by batching.
    void flush() { p_.head.store(p_.writeIdx, std::memory_order_release); }

    T* peek() {
        const size_t t = c_.tail.load(std::memory_order_relaxed);
        if (t == c_.cachedHead) {
            c_.cachedHead = p_.head.load(std::memory_order_acquire); // edge 1
            if (t == c_.cachedHead) return nullptr;
        }
        return &buffer_[t & (Size - 1)];
    }

    void consume() { c_.tail.store(c_.tail.load(std::memory_order_relaxed) + 1, std::memory_order_release); } // edge 2
};
