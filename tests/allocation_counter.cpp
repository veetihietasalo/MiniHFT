// Replaces the global operator new and delete for the test binary, so tests can count heap
// allocations (AllocationCounter.hpp). Every form is replaced, including the aligned ones used
// for over-aligned types such as RingBuffer. The nothrow forms of new call these by default.

#include <cstdlib>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

#include "AllocationCounter.hpp"

namespace {

thread_local bool t_counting = false;
thread_local std::size_t t_count = 0;

void* allocate(std::size_t size) {
    if (t_counting) ++t_count;
    if (void* p = std::malloc(size != 0 ? size : 1)) return p;
    throw std::bad_alloc();
}

void* allocateAligned(std::size_t size, std::align_val_t alignment) {
    if (t_counting) ++t_count;
    const std::size_t align = static_cast<std::size_t>(alignment);
#if defined(_MSC_VER)
    void* p = _aligned_malloc(size != 0 ? size : 1, align);
#else
    const std::size_t rounded = (size + align - 1) / align * align; // aligned_alloc wants a multiple
    void* p = std::aligned_alloc(align, rounded != 0 ? rounded : align);
#endif
    if (!p) throw std::bad_alloc();
    return p;
}

void freeAligned(void* p) noexcept {
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

} // namespace

namespace test_alloc {
    void begin() {
        t_count = 0;
        t_counting = true;
    }
    std::size_t end() {
        t_counting = false;
        return t_count;
    }
}

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, std::align_val_t al) { return allocateAligned(size, al); }
void* operator new[](std::size_t size, std::align_val_t al) { return allocateAligned(size, al); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { freeAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { freeAligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { freeAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { freeAligned(p); }
