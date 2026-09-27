#pragma once

#include <vector>
#include <memory>
#include <cassert>

#include <cstddef>
#include <new>

// Hands out T objects from large blocks and recycles released ones, so steady-state acquire()
// and release() never call the heap. Objects never move: blocks are separate allocations.
// The pool doesn't destroy objects that are still acquired when it is destroyed.
template <typename T, size_t BlockSize = 4096>
class ObjectPool {
    static_assert(alignof(T) <= alignof(std::max_align_t), "blocks are only max_align_t aligned");

private:
    struct Block {
        std::unique_ptr<char[]> memory;
        size_t offset = 0;

        Block() : memory(std::make_unique<char[]>(BlockSize * sizeof(T))) {}
    };

    std::vector<Block> blocks;
    std::vector<T*> freeList;

public:
    ObjectPool() {
        // Allocate first block
        blocks.emplace_back();
    }

    template <typename... Args>
    T* acquire(Args&&... args) {
        if (!freeList.empty()) {
            T* ptr = freeList.back();
            freeList.pop_back();
            new (ptr) T(std::forward<Args>(args)...); // Placement new
            return ptr;
        }

        // Grow first, then take the reference: emplace_back may reallocate `blocks`, and a
        // reference can't be re-pointed afterwards (assigning to it would copy into the old block).
        if (blocks.back().offset >= BlockSize) {
            blocks.emplace_back();
        }
        Block& current = blocks.back();

        T* ptr = reinterpret_cast<T*>(current.memory.get() + current.offset * sizeof(T));
        current.offset++;
        new (ptr) T(std::forward<Args>(args)...);
        return ptr;
    }

    void release(T* ptr) {
        ptr->~T(); // Call destructor
        freeList.push_back(ptr);
    }
};
