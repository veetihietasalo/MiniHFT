# C++ Concepts Explained - MiniHFT Learning Guide

This document explains the C++ concepts, math, and infrastructure used throughout the MiniHFT project.

## C++ Language Features

### 1. Templates (`template <typename T>`)
**Why**: Generic programming - write code once, works with any type.

```cpp
template <typename T>
class Matrix {
    std::vector<T> data;  // Works with int, double, float, etc.
};
```

**Learning**: Templates are compiled separately for each type you use them with. `Matrix<double>` and `Matrix<int>` are different classes.

### 2. Atomics (`std::atomic<T>`)
**Why**: Thread-safe variables without locks.

```cpp
std::atomic<uint64_t> counter{0};
counter.fetch_add(1, std::memory_order_release);  // Thread-safe increment
```

**Memory Orders**:
- `relaxed`: No ordering guarantees (fastest)
- `acquire`: Reads can't move before this
- `release`: Writes can't move after this
- `seq_cst`: Full ordering (slowest)

### 3. `alignas(N)` - Memory Alignment
**Why**: Force variables onto separate cache lines to prevent false sharing.

```cpp
alignas(64) std::atomic<size_t> head;  // Starts at 64-byte boundary
```

**Cache Line**: CPU loads 64 bytes at once. If two threads write to different variables on the same cache line, they invalidate each other's cache.

### 4. `reinterpret_cast<T*>`
**Why**: Treat raw bytes as a specific type (zero-copy parsing).

```cpp
char buffer[36];
AddOrderMsg* msg = reinterpret_cast<AddOrderMsg*>(buffer);
// Now we can access msg->price without copying!
```

**Warning**: Dangerous if alignment/size is wrong. Only use with packed structs.

### 5. `#pragma pack(push, 1)`
**Why**: Remove padding between struct members.

```cpp
#pragma pack(push, 1)
struct Msg {
    char type;     // 1 byte
    uint32_t val;  // 4 bytes - normally would start at offset 4, but now at offset 1
};
#pragma pack(pop)
```

Without packing, compiler adds 3 padding bytes after `type` to align `val` to 4-byte boundary.

## Math & Statistics

### 1. Moving Average
**Formula**: `MA(n) = (x₁ + x₂ + ... + xₙ) / n`

**Use in HFT**: Smooth price data to identify trends.

```cpp
double ma = std::accumulate(prices.begin(), prices.end(), 0.0) / prices.size();
```

### 2. Variance & Standard Deviation
**Variance**: `σ² = Σ(xᵢ - μ)² / n`
**Std Dev**: `σ = √variance`

**Use in HFT**: Measure volatility. High volatility = wider spreads.

### 3. Covariance
**Formula**: `cov(X,Y) = Σ(xᵢ - x̄)(yᵢ - ȳ) / n`

**Use in HFT**: Measure correlation between two assets.

### 4. Avellaneda-Stoikov Model
**Reservation Price**: `r = s - q·γ·σ²·T`
- `s`: Mid price
- `q`: Inventory (positive = long)
- `γ`: Risk aversion
- `σ`: Volatility
- `T`: Time to close

**Spread**: `δ = γ·σ²·T + (2/γ)·ln(1 + γ/k)`

**Learning**: When inventory is positive (long), reservation price drops → want to sell cheaper to reduce risk.

## HFT Infrastructure

### 1. Order Book Structure
**Price-Time Priority**: Orders at best price execute first, ties broken by time.

**Optimization**: Use `std::vector` instead of `std::map` for small books - better cache locality.

### 2. Lock-Free Ring Buffer (Disruptor Pattern)
**Why**: Zero kernel involvement, ~100x faster than mutexes.

**Key Insight**: Producer owns head index, consumer owns tail index. They never write to the same atomic variable.

### 3. ITCH 5.0 Protocol
**Binary Format**: Fixed-size messages (not JSON/XML).
- **Add Order**: 36 bytes
- **Execute Order**: 30 bytes

**Big Endian**: Network byte order (MSB first). x86 is Little Endian (LSB first), so we swap:
```cpp
uint32_t network_val = 0x12345678;
uint32_t host_val = swap32(network_val);  // 0x78563412
```

### 4. Thread Pinning
**Why**: Prevent OS from moving thread to different CPU core (clears L1/L2 cache).

```cpp
SetThreadAffinityMask(GetCurrentThread(), 1 << core_id);
```

### 5. `__rdtsc()` - Read Time-Stamp Counter
**Why**: Measure CPU cycles (nanosecond precision).

```cpp
uint64_t start = __rdtsc();
// ... code to measure ...
uint64_t cycles = __rdtsc() - start;
```

At 3GHz CPU: 1 cycle = 0.33 nanoseconds.

### 6. FPGA Concepts
**Pipeline**: Break computation into stages, each takes 1 clock cycle.
- Stage 1: Fetch
- Stage 2: Decode
- Stage 3: Execute

**Throughput**: Can process 1 message per cycle (after pipeline fills).
**Latency**: 3 cycles to get first result.

**Real FPGA**: Written in Verilog/VHDL, runs on silicon. Our C++ version simulates the concept.

### 7. Kernel Bypass / DPDK
**Traditional Network**: Packet → NIC → Kernel → App (~10μs)
**Kernel Bypass**: Packet → NIC → App memory (~100ns)

**How**: NIC's ring buffer is memory-mapped to user space. App polls memory directly.

## Performance Numbers (Typical)

| Operation | Latency |
|-----------|---------|
| L1 cache hit | ~1 ns |
| L2 cache hit | ~3 ns |
| RAM access | ~100 ns |
| Mutex lock/unlock | ~25 ns (uncontended) |
| Context switch | ~1-10 μs |
| System call | ~100 ns |
| Network round-trip (datacenter) | ~250 μs |

**HFT Goal**: Keep everything in L1/L2 cache, avoid kernel, use lock-free data structures.

## Key Takeaways

1. **Templates**: Generic code, compiled per-type
2. **Atomics**: Lock-free thread safety with memory ordering
3. **Alignment**: Prevent false sharing with `alignas(64)`
4. **Zero-Copy**: Use `reinterpret_cast` on packed structs
5. **Statistics**: Variance/covariance for volatility/correlation
6. **Lock-Free**: Disruptor pattern for inter-thread communication
7. **Hardware**: FPGA pipelines and kernel bypass skip OS overhead
