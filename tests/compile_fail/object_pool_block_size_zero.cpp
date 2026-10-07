// Claim: ObjectPool refuses a block size of 0. acquire() adds a block when the current one is
// full, and a block of 0 slots is always full, so every acquire() would allocate an empty
// block and construct the object past its end.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include <cstddef>

#include "ObjectPool.hpp"

#ifdef MINIHFT_COMPILE_FAIL_CONTROL
constexpr size_t kBlockSize = 1;
#else
constexpr size_t kBlockSize = 0;
#endif

int main() {
    ObjectPool<int, kBlockSize> pool;
    int* value = pool.acquire(42);
    const int seen = *value;
    pool.release(value);
    return seen == 42 ? 0 : 1;
}
