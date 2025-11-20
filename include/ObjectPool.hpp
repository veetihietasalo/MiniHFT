#pragma once

#include <vector>
#include <memory>
#include <cassert>

template <typename T, size_t BlockSize = 4096>
class ObjectPool {
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

        Block& current = blocks.back();
        if (current.offset >= BlockSize) {
            blocks.emplace_back();
            current = blocks.back(); // Update reference
        }

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
