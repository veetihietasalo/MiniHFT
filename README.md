# MiniHFT

Low-latency trading components in C++20, built and measured: a lock-free SPSC queue, a NASDAQ ITCH 5.0 reader, and an order-by-order book that replays a full trading day. A learning project, not for production trading.

[![Build and Test](https://github.com/veetihietasalo/MiniHFT/actions/workflows/build.yml/badge.svg)](https://github.com/veetihietasalo/MiniHFT/actions/workflows/build.yml)

The hot path of a trading system is concurrency, market data and measurement. Here each part is built on its own and every change has a number next to it. Nothing is wired into one trading system, and no strategy runs on the replayed data.

Built with coding agents: 46 of the 57 non-merge commits are authored by Claude and 9 more carry a Claude co-author trailer. The other two are the first commit (Nov 2025) and a LICENSE commit. I set the design, the tests and the measurements.

## Components

- `RingBuffer`: lock-free single-producer, single-consumer queue. Release/acquire on each index, each index on its own cache line, each side caches the other's index.
- ITCH 5.0 reader and L3 book: the reader streams a day's file (or a `gzip -dc` pipe) and returns each message as a span into one reused buffer. The builder keeps one book per symbol: price levels, a FIFO per level, O(1) lookup by order reference.
- MoldUDP64 feed handler (Linux): `mold_send` replays a file as multicast, `mold_recv` takes the A and B feeds, drops duplicates, holds packets past a gap and builds the books.
- Avellaneda-Stoikov market maker (`include/Strategy.hpp`) and a rerun of the paper's section 3.3 simulation.
- Latency harness: TSC timestamps, a log-linear histogram (at most 1.6 % bucket error), thread pinning, `--json` output and `bench/compare.py` for A/B runs.
- Two toy hardware models in plain C++: a 3-stage register pipeline and a NIC receive ring. No HDL, no real NIC.
- Checks: 165 tests in the `gcc-release` preset, compile-fail tests, ThreadSanitizer, ASan + UBSan, a libFuzzer target for the ITCH parser, clang-tidy, and ARM64 CI jobs.

## Results

Ryzen 9 9900X3D, Windows 11 and WSL2. Latencies include about 10 ns of timer overhead. Methods and caveats are in `docs/`.

| What | Result |
|------|--------|
| One hop between two pinned threads, same CCD | 50 ns p50, 71 ns p99 (`ring_latency`, MSVC, high priority) |
| The same hop across CCDs | 171-180 ns p50, 320-331 ns p99 |
| Queue, back-to-back | 6.5-7.0 ns per message (150 M msg/s). The original `fetch_add` queue: 38-39 ns |
| Book update, L3 book | 20 ns p50, flat from 10 to 1,000 resting orders per side. The original sorted-vector book: take 131 to 1,005 ns |
| Full NASDAQ day, 30 Dec 2019 | 268.7 M messages at 2.92-2.98 M msg/s, gzip included. Book update p50 131 ns, p99 495-502 ns. 0 events for unknown orders, 0 orders left at the close |
| Heap allocations once warm | 0 in the queue, the pool release, the histogram and the ITCH to book path |
| Relaxed `publish()`, broken on purpose | 0 damaged in 10M messages on x86. Damaged messages in every 8-slot run so far on Arm Neoverse N2 |
| Avellaneda-Stoikov, 1,000 runs | Inventory strategy profit 64.6, std 6.6 (paper 65.0, 6.6). Symmetric 67.6, 13.2 (paper 68.4, 12.7) |

All latencies are from Windows or WSL2. A native Linux baseline is next ([setup](docs/native_linux.md)).

## Build and run

Needs CMake 3.25+ and GCC 13+ (Linux) or Visual Studio 2022 (Windows). GoogleTest and Google Benchmark are fetched on configure.

```bash
cmake --preset gcc-release          # msvc-release on Windows
cmake --build --preset gcc-release
ctest --preset gcc-release
./build/gcc-release/MiniHFT         # the Avellaneda-Stoikov simulation
./build/gcc-release/orderbook_latency
./build/gcc-release/ring_latency
```

On GCC 13.3 (Ubuntu 24.04, WSL2) the 165 tests pass and the simulation prints profits of 64.59 and 67.56. The full-day replay needs NASDAQ's free sample file (`12302019.NASDAQ_ITCH50.gz` from https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/):

```bash
gzip -dc 12302019.NASDAQ_ITCH50.gz | ./build/gcc-release/itch_replay -
```

Other presets: `clang-tsan`, `clang-asan-ubsan`, `clang-fuzz`, `gcc-debug-stl`. On Windows the programs are in `build/msvc-release/Release/`.

## What I learned

- A memory-ordering bug can pass 10M messages on x86 and still be a bug. A copy of the queue with a relaxed `publish()` damaged nothing on x86 and damaged messages in every 8-slot run so far on Arm. ThreadSanitizer flagged it on both.
- Each index has one writer, so a release store replaces `fetch_add`. That took the queue from 38-39 to 6.5-7.0 ns per message. Cached indices only paid off (14-19 %) when one side did work between calls.
- Latency measured from the actual send time hides stalls. Measured from the scheduled send time, p99.9 reaches several ms (coordinated omission).
- A real day costs about 6x the synthetic benchmark per book update (131 ns against 20 ns). The replay touches thousands of books and a 2-million-order working set.
- Fuzzing found what the unit tests missed: a short ITCH message was read past its end (heap overflow after 73 inputs). Six bugs in all, each with a failing test before its fix ([code_quality.md](docs/code_quality.md)).
- Skipping ahead at the first packet gap made the B feed useless. The receiver holds packets and gives up a gap only after a wait, once every socket is drained.

## Not done

- No backtester, and no strategy runs on replayed market data.
- Kernel bypass is a model of a receive ring, not a driver. `Network.hpp` is Winsock only, the multicast code is POSIX only, and macOS is not supported.
- `learning_roadmap.md` and `docs/cpp_learning_guide.md` are early notes. Where they disagree with the code, the code is right.

Docs: [memory ordering](docs/memory_ordering.md), [RingBuffer v2](docs/ring_buffer_v2.md), [L3 order book](docs/order_book.md), [code quality](docs/code_quality.md), [verification](docs/verification.md), [networking](docs/networking.md).

MIT, see [LICENSE](LICENSE).
