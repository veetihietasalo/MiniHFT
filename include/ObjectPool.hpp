#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <new>
#include <utility>
#include <vector>

// Hands out T objects from large blocks and recycles released ones. Objects never move: every
// block is its own allocation.
//
// acquire() calls the heap only when every block is in use and a new one is needed. release()
// never does: released slots form an intrusive free list, where a released slot's own memory
// holds the pointer to the next free slot, so there is no side container that could need to
// grow. The pool doesn't destroy objects that are still acquired when it is destroyed.
template <typename T, size_t BlockSize = 4096>
class ObjectPool {
    static_assert(BlockSize > 0);

    struct FreeSlot {
        FreeSlot* next;
    };

    // Every slot must be able to hold either a T or a FreeSlot.
    static constexpr size_t kAlign = std::max(alignof(T), alignof(FreeSlot));
    static constexpr size_t kStride = (std::max(sizeof(T), sizeof(FreeSlot)) + kAlign - 1) / kAlign * kAlign;
    static_assert(kAlign <= __STDCPP_DEFAULT_NEW_ALIGNMENT__, "blocks only get operator new[]'s default alignment");

public:
    ObjectPool() { addBlock(); }
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // Constructs a T in a free slot: the most recently released one, or the next unused one.
    template <typename... Args>
    [[nodiscard]] T* acquire(Args&&... args) {
        void* slot = nullptr;
        if (freeHead_ != nullptr) {
            slot = freeHead_;
            freeHead_ = freeHead_->next;
        } else {
            if (used_ == BlockSize) addBlock();
            slot = blocks_.back().get() + used_ * kStride;
            ++used_;
        }
        return ::new (slot) T(std::forward<Args>(args)...);
    }

    // Destroys the object and puts its slot at the front of the free list.
    void release(T* object) noexcept {
        object->~T();
        freeHead_ = ::new (static_cast<void*>(object)) FreeSlot{freeHead_};
    }

private:
    void addBlock() {
        blocks_.push_back(std::make_unique<std::byte[]>(BlockSize * kStride));
        used_ = 0;
    }

    std::vector<std::unique_ptr<std::byte[]>> blocks_;
    size_t used_ = 0;              // slots handed out from the newest block
    FreeSlot* freeHead_ = nullptr; // most recently released slot
};
