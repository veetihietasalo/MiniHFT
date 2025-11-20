# Learning Roadmap: C++, HPC, & HFT

This roadmap guides you through building the MiniHFT engine while mastering the underlying concepts.

## Phase 1: The Modern C++ Foundation
**Goal**: Write safe, expressive, and fast C++.
- [ ] **RAII (Resource Acquisition Is Initialization)**: Understand `std::unique_ptr`, `std::shared_ptr`, and destructors. *Why?* No manual `delete`, no leaks.
- [ ] **Templates & Metaprogramming**: Write generic code (like our `Matrix` class) that works for any type.
- [ ] **Move Semantics**: `std::move` and rvalue references. *Why?* Avoid expensive copies of large data structures.

## Phase 2: High-Performance Computing (HPC)
**Goal**: Squeeze every cycle out of the CPU.
- [ ] **Memory Layout**: Row-major vs Column-major. Cache locality. *Why?* A cache miss costs ~100x more than an instruction.
- [ ] **SIMD (Single Instruction, Multiple Data)**: Doing 4 or 8 math operations at once.
- [ ] **Branch Prediction**: Why `if` statements can be slow inside tight loops.
- [ ] **Profiling**: Using tools (Visual Studio Profiler, VTune) to find bottlenecks.

## Phase 3: The Math of Markets
**Goal**: Understand what the numbers mean.
- [ ] **Linear Algebra**: Dot products for correlation, Eigenvalues for PCA (Principal Component Analysis).
- [ ] **Statistics**: Normal distribution, Standard Deviation (Volatility), Z-Scores.
- [ ] **Stochastic Processes**: Random Walks, Brownian Motion (modeling stock prices).
- [ ] **Inventory Risk**: Avellaneda-Stoikov model. How holding too much stock increases risk.

## Phase 4: HFT Architecture
**Goal**: Low latency, high throughput.
- [ ] **Order Books**: How exchanges store data (Price-Time Priority).
- [ ] **Lock-Free Programming**: `std::atomic`. Avoiding mutexes to prevent thread blocking.
- [ ] **Kernel Bypass (Advanced)**: Mentioning how real HFTs skip the OS network stack (Solarflare, DPDK).
- [ ] **Networking**: TCP vs UDP (Multicast). Sockets programming.

## Resources
- **Books**: "Effective Modern C++" (Scott Meyers), "Low Latency C++" (Sourav Ghosh).
- **Sites**: cppreference.com, Godbolt Compiler Explorer.
