# Code quality: six bugs, harder APIs, and the checks that keep them out

After W05 the code was reviewed as if for production. That turned up six bugs. Each one got a test that failed before its fix and passes after it. The APIs that made those bugs easy to write were tightened. CI then gained the tools that would have caught each bug on its own: sanitizers, a fuzzer, warnings as errors and clang-tidy.

## Six bugs, each proven before it was fixed

| # | Bug | How it showed | Fix |
|--:|-----|---------------|-----|
| 1 | **Malformed ITCH messages reached the decoders.** The reader returned whatever length the 2-byte prefix gave, and the decoders read fixed offsets. A message shorter than its type's layout was read past its end. | libFuzzer + AddressSanitizer: heap-buffer-overflow in `itch::be16` after **73 inputs** | The reader skips and counts frames whose length doesn't match their type. `dispatch()` takes a `std::span` and refuses a wrong length. |
| 2 | **A reused order reference left an order nobody could reach.** Adding a reference that was still live overwrote its index entry. The first order stayed queued at its price level but could never be executed or deleted. A replace onto a live reference did the same. | `L3Book.DuplicateAddLeavesNoUnreachableOrder` and `…ReplaceOntoALiveReference…` failed on MSVC and GCC | `OrderIndex::tryInsert` refuses a key that's already there. The builder drops the new order and counts it in `duplicateRefs`, which `itch_replay` reports. |
| 3 | **`pinThread(64)` on Windows pinned the thread to core 0.** `DWORD_PTR{1} << coreId` is undefined for shifts of 64 or more, or negative ones. On x86 the shift count wraps, so core 64 became core 0, silently. | `ThreadUtils.PinThreadRejectsNegativeAndHugeCoreIds` failed on MSVC | Range check against the affinity mask's width. Linux already checked against `CPU_SETSIZE`. |
| 4 | **Input ending inside a length prefix wasn't reported.** One leftover byte at the end of a file was treated as a clean end of input. | `Itch.ReaderFlagsInputEndingInsideALengthPrefix` failed on both | `truncated()` is set whenever bytes are left over. |
| 5 | **`valueAtPercentile(NaN)` was undefined behaviour.** `NaN <= 0` is false and `std::min(NaN, 100.0)` is NaN, so the NaN reached a double-to-integer conversion. | `LatencyHistogram.NanPercentileReturnsTheMinimum` failed on MSVC. GCC happened to return the right value. | `if (!(percentile > 0.0)) return min_;` catches NaN as well as values ≤ 0. |
| 6 | **Reference `2⁶⁴−1` broke the order index.** That value marks an empty slot. Inserting it wrote a slot that still read as empty but counted it in `size()`, and `find()` then returned an empty slot's value. | `OrderIndex.ReservedKeyIsRefusedAndNeverFound` failed on both | `tryInsert` refuses the reserved key; `find` and `erase` report it as absent. |

Before the fixes, the new tests failed **6 on MSVC and 4 on GCC**. The two extra MSVC failures are bug 3, which is Windows-only code, and bug 5, where GCC's result was luck.

NASDAQ's real feed hits none of the parser and book bugs (1, 2, 4 and 6). The full 30 Dec 2019 day, replayed through the hardened reader, gives the same results as before:
- 268,744,780 messages;
- 0 wrong lengths, unknown types, reused live references and unknown orders;
- the same 4,738 crossed books;
- a 130.6 ns median book update.

The bugs matter for what a corrupted file, a replay bug or a fuzzer can send: bad input must be rejected, not corrupt the book.

## APIs that make those bugs harder to write

- **`std::span` in the parser.** `ItchReader::next()` returns a `std::span<const uint8_t>`, empty at the end of input, and only ever a message whose length matches its type. `itch::dispatch(span, handler)` checks the length again and returns whether it dispatched. A decoder can no longer receive a pointer without a length.
- **`[[nodiscard]]`** on every query and every call that can fail: `pinThread`, `claim`, `peek`, `find`, `tryInsert`, `acquire`, `itch::dispatch`, the histogram's percentiles, the TSC readers. Ignoring one is a warning, and with warnings as errors, a build failure. Compile-fail tests prove it for ten of them ([verification.md](verification.md#compile-fail-tests)), and found `dispatch` missing from the list. The benchmarks that used to ignore `pinThread` now say when pinning failed, since their numbers mean less then.
- **An intrusive free list in `ObjectPool`.** A released slot stores the next free slot's pointer in its own, now unused, bytes. `release()` used to push onto a `std::vector<T*>`, which allocates whenever it grows: **13 allocations to release 3,000 objects, now 0** (`ZeroAlloc.ObjectPoolReleaseNeverAllocates`). A burst of cancels no longer touches the heap.
- **`OrderIndex::tryInsert` never overwrites** (bug 2). `find()` documents that its pointer is invalidated by the next insert, which may rehash.

## Warnings as errors

GCC and Clang build our code with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wdouble-promotion -Wnon-virtual-dtor -Woverloaded-virtual -Wformat=2 -Wimplicit-fallthrough -Wnull-dereference`, plus `-Wduplicated-cond -Wlogical-op` on GCC. MSVC builds it with `/W4`. Every preset turns on `MINIHFT_WARNINGS_AS_ERRORS` (`-Werror` or `/WX`). GoogleTest and Google Benchmark are included as system headers, so their code can't fail our build.

The first strict build gave **28 warnings**. Most were implicit sign or width conversions in the older code. Two were real:
- `-Wnull-dereference`: `orderbook_latency` dereferenced `bestAsk()` without checking that the side had orders.
- Clang's `-Wunused-but-set-variable` in `hw_bench`: the timed loop's sum was never used, so the optimizer was free to delete the work being timed. It now prints the sum as a checksum.

One exception remains: the test binary is built without `-Wnull-dereference`. GCC reports it inside GoogleTest's assertion macros after inlining, even though they come from a system header.

**Every header compiles on its own.** CMake generates one source file per header in `include/` that includes only that header (target `minihft_header_check`). A header that relies on its includer's includes fails there. Headers that no program includes still get compiled under the same warnings.

## Sanitizers

| Preset | Checks | Tests |
|--------|--------|------:|
| `clang-asan-ubsan` | AddressSanitizer + UndefinedBehaviorSanitizer; leak detection on | 69 |
| `clang-tsan` | ThreadSanitizer, including the test that passes only if TSan reports the deliberately broken queue (W02) | 70 |

Both build with `-fno-sanitize-recover=all`, so the first report ends the run and a test can't pass after one. Sanitizer runtimes replace global `operator new`, and so do the zero-allocation tests, so those 8 tests only run in the other builds.

## Fuzzing

[`tests/fuzz/itch_fuzz.cpp`](../tests/fuzz/itch_fuzz.cpp) feeds arbitrary bytes through `ItchReader` into a book builder. The reader's buffer is only 16 bytes, which forces refills in the middle of messages. The raw input also goes straight to `dispatch()`. It builds with `-fsanitize=fuzzer,address,undefined` (preset `clang-fuzz`).

- **On the old code:** a heap-buffer-overflow after 73 inputs (bug 1).
- **On the fixed code:** 15,320,086 inputs in 120 s, about 127,000 a second, reaching 2,964 new coverage units, with no crash.

The input that found bug 1 is kept in [`tests/fuzz/corpus/`](../tests/fuzz/corpus/), so every fuzzing run replays it first.

## clang-tidy

[`.clang-tidy`](../.clang-tidy) enables these check families, and any finding is an error:
- bug-finding: `bugprone-*`, `concurrency-*`, `performance-*`, `misc-*`;
- a few `modernize-*`, `readability-*` and `cppcoreguidelines-*` checks that catch mistakes rather than enforce a style.

The first run found **158 findings** in 21 files:

| Result | Checks | Findings |
|--------|--------|---------:|
| Fixed | missing or unused includes (`misc-include-cleaner`): 45 once all 24 files were checked | 40 |
| Fixed | file-local functions and globals given internal linkage | 6 |
| Fixed | `strerror` and `rand` are not thread-safe. `pinThread` runs on both ring-buffer threads at once; it now uses `std::generic_category().message()`. | 3 |
| Fixed | duplicate `#include`s | 2 |
| Fixed | `RingV2::publish` had two identical branches (`bugprone-branch-clone`); now one store after an early return | 2 |
| Fixed | `rewind()` with no error check in the parser tests: now `fseek`, and the temporary file is closed by RAII even when an `ASSERT` returns early | 1 |
| Fixed | `Side` used an `int` where one byte does; an implicit widening in CPUID indexing | 2 |
| Configured | `misc-non-private-member-variables-in-classes` now ignores plain structs. That left one finding, the GoogleTest fixture member, marked `NOLINT`. | 36 |
| Turned off | `misc-const-correctness` and `bugprone-easily-swappable-parameters` (reasons in `.clang-tidy`) | 66 |

Analysing all 24 files also found a `static` function that belonged in an anonymous namespace and a missing `reserve`. The result is **0 findings in all 24 source files**, run through the `clang-tidy` preset. That preset configures every target, the fuzzer included, with no sanitizer, only to produce `compile_commands.json`.

CI uses Ubuntu 24.04's clang-tidy 18. A check newer than that (`misc-use-internal-linkage` arrived in 19) runs only with a newer local clang-tidy.

## What CI runs on every push

| Job | What must hold |
|-----|----------------|
| `msvc-release` | MSVC `/W4 /WX` build; 105 tests: 77 unit tests, 27 [compile-fail tests](verification.md#compile-fail-tests) and `compare.py`'s tests |
| `gcc-release` | GCC strict warnings with `-Werror`; the same 105 tests |
| `clang-tsan` | ThreadSanitizer; 71 tests (no compile-fail tests under a sanitizer) |
| `clang-asan-ubsan` | AddressSanitizer + UndefinedBehaviorSanitizer; 70 tests |
| `fuzz` | 60 s of libFuzzer without a crash, starting from the saved inputs and `gen_itch`'s sample feed |
| `clang-tidy` | 0 findings |
| `bench-compare` | Report only: benchmarks the merge-base and the head on the same runner and writes the comparison to the job summary ([verification.md](verification.md#benchmark-baselines)). A regression raises a warning, never a failure. |

## Reproduce

```bash
cmake --preset clang-asan-ubsan && cmake --build --preset clang-asan-ubsan && ctest --preset clang-asan-ubsan

cmake --preset clang-fuzz && cmake --build --preset clang-fuzz
mkdir corpus && cp tests/fuzz/corpus/* corpus/ && (cd corpus && ../build/clang-fuzz/gen_itch)
./build/clang-fuzz/itch_fuzz -max_total_time=60 corpus

cmake --preset clang-tidy
jq -r '.[].file' build/clang-tidy/compile_commands.json | grep -E "^$PWD/(src|bench|tests)/" \
  | xargs -d '\n' -P "$(nproc)" -n 2 clang-tidy -p build/clang-tidy --quiet
```

If a sanitizer build dies at startup with `unexpected memory mapping`, the kernel's ASLR entropy is too high for Clang 18's runtimes. Run `sudo sysctl vm.mmap_rnd_bits=28` (CI does), or `setarch "$(uname -m)" -R ctest --preset …` without root.

## Still open

- **Too few values give 0 rather than an error.** `mean()` of nothing and `variance()` of one value return 0, while `covariance()` throws (`Statistics.MeanOfNothingAndVarianceOfOneValueAreZero` pins this). With a volatility of 0 from one sample, Avellaneda-Stoikov quotes its narrowest spread with no inventory skew. Pick one contract before a strategy uses these.
- **Three calls that can fail still aren't `[[nodiscard]]`:** `OrderIndex::erase`, `FpgaPipeline::output` and `Network::connectToServer`. Making `erase` one means first deciding what `ItchBook::removeOrder` should do when it fails.
- **MSVC doesn't check initializer order.** Its C5038 is off by default, even at `/W4`, so the `Matrix` bug below failed only the GCC and Clang builds. `/w15038` would turn it on.

### Closed: the three headers nothing used

`Matrix.hpp`, `Statistics.hpp` and `KernelBypass.hpp` used to be listed here. The header check compiled them, but a template's body is only compiled for the types something instantiates, and nothing did. [`tests/matrix_statistics_test.cpp`](../tests/matrix_statistics_test.cpp) and [`tests/kernel_bypass_test.cpp`](../tests/kernel_bypass_test.cpp) now use all three. `Matrix<int>` and `Matrix<double>` are instantiated explicitly, so every member is compiled, `print()` included. Each problem was shown on the old headers before it was fixed:

| Bug | How it showed | Fix |
|-----|---------------|-----|
| **`Matrix` listed its initializers out of order.** `data` is declared first but was initialized after `rows` and `cols`. | `-Wreorder` on GCC and Clang, an error with `-Werror` | Initializers in declaration order |
| **`Matrix(rows, cols)` didn't check `rows * cols` for overflow.** A 2⁶³ × 2 matrix wrapped to 0 elements and allocated nothing, yet `m(0, 0)` passed the bounds check. | `Matrix.DimensionsWhoseProductOverflowsAreRefused` failed. Writing `m(0, 0)`: UBSan, reference binding to null pointer | The constructor throws `std::length_error` |
| **`movingAverage` divided `T` by `size_t`.** With integer data that division was unsigned: the window {−2, −4} of `int64_t` averaged to 9,223,372,036,854,775,805. | `-Wconversion` on GCC and `-Wimplicit-int-float-conversion` on Clang, for `double` and `float` | The window size is converted to `T`. All five templates now require `std::floating_point`: an integer mean would be truncated (the mean of {1, 2} was 1), so integer data no longer compiles. `return 0.0` became `return T{}`. |
| **`KernelBypass` signalled with a `volatile bool` and fences.** Fences only synchronize through an atomic object, so the flag itself was a data race. | `KernelBypass.TwoThreadsDeliverEveryPacketInOrder` under clang-tsan, within the first lap: `poll()` reading `ready` races with `nicReceive()` writing it | `ready` is a `std::atomic<bool>`: release store in `nicReceive()`, acquire load in `poll()` |
| **`nicReceive()` overwrote packets the CPU hadn't polled.** With only the fix above, a NIC that laps the CPU rewrites a slot the CPU may still be reading, and packets are lost. | `KernelBypass.FullRingRefusesPacketsUntilTheCpuPolls`: the 1,025th packet came out in place of packet 0 | The return handoff, as in `RingBuffer`: `poll()` clears `ready` with a release store, and `nicReceive()` checks it with an acquire load. It returns `false` while the slot is full, as a real NIC drops a packet when it has no free descriptor. |
