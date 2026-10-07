// Claim: ObjectPool refuses a type aligned more strictly than operator new[] guarantees
// (__STDCPP_DEFAULT_NEW_ALIGNMENT__, 16 bytes on x86-64). Its blocks are plain byte arrays from
// operator new[], so a cache-line-aligned order could be constructed at a misaligned address.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include <cstddef>
#include <cstdint>

#include "ObjectPool.hpp"

namespace {

#ifdef MINIHFT_COMPILE_FAIL_CONTROL
constexpr size_t kOrderAlign = alignof(uint64_t);
#else
constexpr size_t kOrderAlign = 64; // one order per cache line
#endif

struct alignas(kOrderAlign) Order {
    uint64_t ref = 0;
    uint32_t shares = 0;
};

} // namespace

int main() {
    ObjectPool<Order, 64> pool;
    Order* order = pool.acquire();
    const bool fresh = order->ref == 0;
    pool.release(order);
    return fresh ? 0 : 1;
}
