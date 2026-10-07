// Claim: ObjectPool::acquire()'s result can't be ignored. The pointer is the only way to
// release the object, so dropping it loses the slot for good. Ignoring it is a warning, and
// with warnings as errors a build failure.
//
// Compile-fail test (cmake/CompileFailTests.cmake): the misuse below must not compile.
// MINIHFT_COMPILE_FAIL_CONTROL swaps in the correct use, which must.

#include <cstdint>

#include "ObjectPool.hpp"

int main() {
    ObjectPool<uint64_t, 64> pool;
#ifdef MINIHFT_COMPILE_FAIL_CONTROL
    uint64_t* order = pool.acquire(uint64_t{42});
    pool.release(order);
#else
    pool.acquire(uint64_t{42}); // nothing can release this slot now
#endif
    return 0;
}
