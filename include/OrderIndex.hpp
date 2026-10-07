#pragma once

// Order reference number -> Value, for finding an order in O(1) when the feed executes,
// cancels or replaces it.
//
// Open addressing with linear probing in one flat array: a lookup is a hash and usually a
// single cache line, where std::unordered_map would chase a pointer to a separately allocated
// node. Deletion uses backward shifting instead of tombstones, so the table never fills with
// dead entries during a trading day that adds and removes hundreds of millions of orders.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

template <typename Value>
class OrderIndex {
public:
    static constexpr uint64_t kEmpty = ~uint64_t{0}; // marks unused slots, so it can never be a key

    explicit OrderIndex(size_t expectedEntries = 1024) { rehash(capacityFor(expectedEntries)); }

    // A moved-from index is empty and can be used again: it has no slots until the next
    // tryInsert() allocates them. The implicit moves would take the slots but keep the old size,
    // mask and shift, and the next call on the source would index an empty table.
    OrderIndex(const OrderIndex&) = default;
    OrderIndex& operator=(const OrderIndex&) = default;
    OrderIndex(OrderIndex&& other) noexcept
        : slots_(std::move(other.slots_)), mask_(other.mask_), shift_(other.shift_), size_(other.size_) {
        other.becomeEmpty();
    }
    OrderIndex& operator=(OrderIndex&& other) noexcept {
        if (this != &other) {
            slots_ = std::move(other.slots_);
            mask_ = other.mask_;
            shift_ = other.shift_;
            size_ = other.size_;
            other.becomeEmpty();
        }
        return *this;
    }
    ~OrderIndex() = default;

    [[nodiscard]] size_t size() const { return size_; }
    [[nodiscard]] size_t capacity() const { return slots_.size(); }

    // Value for `key`, or nullptr if absent. The pointer is valid until the next tryInsert(),
    // which may rehash the table.
    [[nodiscard]] Value* find(uint64_t key) {
        if (key == kEmpty || size_ == 0) return nullptr; // size_ == 0: a moved-from index has no slots
        for (size_t i = home(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.key == key) return &s.value;
            if (s.key == kEmpty) return nullptr;
        }
    }

    // Adds key -> value. Returns false, changing nothing, if the key is already present or is
    // kEmpty: an existing entry is never overwritten.
    [[nodiscard]] bool tryInsert(uint64_t key, const Value& value) {
        if (key == kEmpty) return false;
        // Keeps the load factor <= 0.5: twice the slots when full, 16 for a moved-from index.
        if ((size_ + 1) * 2 > slots_.size()) rehash(capacityFor(size_ + 1));
        for (size_t i = home(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.key == key) return false;
            if (s.key == kEmpty) {
                s = Slot{key, value};
                ++size_;
                return true;
            }
        }
    }

    // Removes `key`; returns false if it wasn't there.
    bool erase(uint64_t key) {
        if (key == kEmpty || size_ == 0) return false; // size_ == 0: a moved-from index has no slots
        size_t hole = home(key);
        while (slots_[hole].key != key) {
            if (slots_[hole].key == kEmpty) return false;
            hole = (hole + 1) & mask_;
        }
        // Backward shift: walk the cluster after the hole and move back any entry whose home
        // slot is not between the hole and its current position, so every entry stays
        // reachable from its home without tombstones.
        for (size_t j = (hole + 1) & mask_; slots_[j].key != kEmpty; j = (j + 1) & mask_) {
            const size_t h = home(slots_[j].key);
            const bool stays = (hole < j) ? (hole < h && h <= j) : (hole < h || h <= j);
            if (!stays) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].key = kEmpty;
        --size_;
        return true;
    }

private:
    struct Slot {
        uint64_t key = kEmpty;
        Value value{};
    };

    void becomeEmpty() noexcept {
        slots_.clear(); // a moved-from vector is empty already; this makes it certain
        size_ = 0;
    }

    static size_t capacityFor(size_t entries) {
        size_t c = 16;
        while (c < entries * 2) c *= 2;
        return c;
    }

    // Fibonacci hashing: multiply, keep the top bits. Spreads sequential reference numbers evenly.
    [[nodiscard]] size_t home(uint64_t key) const { return static_cast<size_t>((key * 0x9E3779B97F4A7C15ULL) >> shift_); }

    void rehash(size_t newCapacity) {
        // A power of two, at least 16 (capacityFor), so shift_ ends up at most 60 and home()
        // never shifts by 64. Stated here because the static analyzer can't follow capacityFor's
        // loop and would otherwise assume any capacity.
        assert(newCapacity >= 16 && (newCapacity & (newCapacity - 1)) == 0);
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(newCapacity, Slot{});
        mask_ = newCapacity - 1;
        shift_ = 64;
        for (size_t c = newCapacity; c > 1; c >>= 1) --shift_;
        size_ = 0;
        for (const Slot& s : old) {
            if (s.key != kEmpty) placeUnique(s.key, s.value);
        }
    }

    // For rehash: the key is known to be absent and the table to have room.
    void placeUnique(uint64_t key, const Value& value) {
        size_t i = home(key);
        while (slots_[i].key != kEmpty) i = (i + 1) & mask_;
        slots_[i] = Slot{key, value};
        ++size_;
    }

    std::vector<Slot> slots_;
    size_t mask_ = 0;
    unsigned shift_ = 64;
    size_t size_ = 0;
};
