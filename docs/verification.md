# Verification: checks for the C++ an AI gets wrong

Most of MiniHFT is written with an AI assistant. An AI is reliable in some areas of C++ and confidently wrong in others, so every area where it is unreliable gets a check that doesn't trust the author, human or AI:

| Area | How it goes wrong | What checks it | Where it runs |
|------|-------------------|----------------|---------------|
| Syntax, idioms, standard library | Rarely | Warnings as errors on three compilers, clang-tidy, and every header compiling on its own | CI ([code_quality.md](code_quality.md)) |
| Templates, concepts, overload resolution | A confident claim such as "this concept rejects X" that isn't true | [Compile-fail tests](#compile-fail-tests) | CI, every build without a sanitizer |
| Lifetimes, aliasing, undefined behaviour | Code that follows the rules on paper and breaks them in real use | AddressSanitizer + UndefinedBehaviorSanitizer and fuzzing, plus [libstdc++ debug mode, stricter UBSan and the Clang Static Analyzer](#lifetimes-and-undefined-behaviour-the-gaps-asan-leaves) | CI |
| Memory ordering, lock-free code | Plausible but wrong orderings. On x86 these often compile to the same instructions as the right ones. | [ThreadSanitizer on every threaded path](#memory-ordering-every-threaded-path-more-schedules), SPSC stress under three thread schedules, and two tests that pass only if TSan catches a deliberately broken queue | CI ([memory_ordering.md](memory_ordering.md)) |
| Performance claims | Reasoning about caches and branches without measuring | [Benchmark baselines](#benchmark-baselines): JSON results, `compare.py`, saved baselines, and an A/B job in CI | The benchmark machine; CI only reports |

## Compile-fail tests

A `static_assert(!ItchHandler<X>)` shows what a trait answers. It doesn't show that a real call with `X` is refused. The compile-fail tests show exactly that: a call, an instantiation or an ignored `[[nodiscard]]` result must fail to compile, and fail for the expected reason.

**How a test works.** [`cmake/CompileFailTests.cmake`](../cmake/CompileFailTests.cmake) builds each file in [`tests/compile_fail/`](../tests/compile_fail/) twice:

- **As written,** in a target that isn't part of the normal build. Its ctest test builds that target. The test passes only if the build fails **and** the compiler output matches the expected reason: the concept's name, the `static_assert` message, or the warning's name. A build that succeeds can't pass, however it matches.
- **With `MINIHFT_COMPILE_FAIL_CONTROL` defined,** in the normal build. That swaps the misuse for its correct form, so the file must compile. A typo or a missing include then breaks the build, instead of passing the test by failing for the wrong reason.

The tests run one at a time, because they build inside the build tree, while the unit tests keep running in parallel. The broken targets are left out of `compile_commands.json`, so clang-tidy never sees them; it analyses the controls instead. Sanitizer builds skip the tests, since a compile error doesn't depend on the sanitizer.

**What they cover:** 28 tests, on GCC, Clang and MSVC.

| Kind | Tests | What each proves |
|------|------:|------------------|
| Concepts | 9 | `itch::dispatch` refuses a handler that is missing a method, or one that takes the wrong message type. `BasicItchBookBuilder` refuses a listener with the wrong method, or one whose `onTopOfBook` returns a value. All five `Statistics` templates refuse integer data. |
| `static_assert`s | 8 | `RingBuffer` and the three ring variants refuse a size that isn't a power of two, and a size of 0. `RingV2` refuses a batch larger than the ring. `ObjectPool` refuses an over-aligned type, and a block size of 0. |
| Deleted functions | 1 | `OrderIndex` can't be moved ([why](#the-first-fix-cost-2-ns-on-the-hot-path)); the control copies it. |
| `[[nodiscard]]` | 10 | Under warnings-as-errors, ignoring the result of any of these calls fails the build: `pinThread`, `claim`, `peek`, `tryInsert`, `find`, `acquire`, `nicReceive`, `poll`, `ItchReader::next` and `dispatch`. |

The expected reasons name the constraint, not one compiler's wording, so the same patterns hold for all three compilers. MSVC can only be tested in CI. Its first run there passed the first 27, in 32 s.

### Two bugs the tests found

On the old headers, 6 of the first 27 failed on GCC:

| Bug | How it showed | Fix |
|-----|---------------|-----|
| **A ring of size 0 compiled.** `(Size & (Size - 1)) == 0` is true for 0, so `RingBuffer<int, 0>` passed the power-of-two check. It built into a queue that is always full and always empty (`claim()` and `peek()` both return null). The same check was in `RingV0`, `RingV1` and `RingV2`. | 4 tests failed for the wrong reason. GCC refused the zero-size array, but only because of `-Wpedantic` under `-Werror`; `RingV2` was refused only by its batch check. Without warnings-as-errors (the CMake default), it compiled with a warning. | `Size > 0 &&` in all four checks. |
| **`itch::dispatch` wasn't `[[nodiscard]]`.** It returns `false` for a message whose length is wrong for its type, and `itch_replay` dropped that result. | `DispatchResultMustBeUsed`: the misuse compiled. | `[[nodiscard]]`. `itch_replay` casts the result to `void` with a comment: `ItchReader` only returns well-formed messages. |

The other two failures were missing diagnostics, not bugs: `ObjectPool`'s `static_assert(BlockSize > 0)` had no message for a test to match. It has one now.

### Adding one

Write the misuse in `tests/compile_fail/<name>.cpp`. Start with a comment stating the claim it proves, and put the correct form under `#ifdef MINIHFT_COMPILE_FAIL_CONTROL`. Then register it in the block at the end of `cmake/CompileFailTests.cmake`:

```cmake
minihft_compile_fail_test(NAME RingBufferRejectsSizeZero
    SOURCE ${dir}/ring_buffer_size_zero.cpp EXPECT "${power_of_two}")
```

Add `WARNING` for a misuse that only fails because of warnings-as-errors, such as an ignored `[[nodiscard]]` result. Those tests exist only when `MINIHFT_WARNINGS_AS_ERRORS` is on, which every preset sets. To run just this group: `ctest --preset gcc-release -L compile-fail`.

## Lifetimes and undefined behaviour: the gaps ASan leaves

AddressSanitizer sees memory, not container rules or paths that tests don't take. Four additions, all in CI:

| Tool | What it adds | Result |
|------|--------------|--------|
| **`gcc-debug-stl` preset** | libstdc++ debug mode (`_GLIBCXX_DEBUG`, `_GLIBCXX_DEBUG_PEDANTIC`, `_GLIBCXX_ASSERTIONS`) on every target, GoogleTest included, since debug mode changes the containers' layout. It catches an invalidated iterator, or `operator[]` past `size()` but inside the capacity. ASan misses both, because the memory is still allocated. | 0 reports in the tests and programs, and in 60 s of fuzzing with debug mode on. The zero-allocation tests still pass, so debug mode doesn't allocate on the hot paths. |
| **Stricter UBSan** | `-fsanitize=undefined` leaves out `float-divide-by-zero`, `local-bounds`, `implicit-conversion` and `nullability`; `clang-asan-ubsan` and `clang-fuzz` now add them. `implicit-conversion` adds the narrowing that `-Wconversion` can't see, such as `+=` on a narrow type. | 0 reports in our code. `implicit-conversion` fired only inside libstdc++ (`uniform_int_distribution<int>`), which [`sanitizer-ignorelist.txt`](../sanitizer-ignorelist.txt) exempts along with the fetched dependencies. `nullability` checks nothing yet, since there are no `_Nonnull` annotations; it's there for future ones. |
| **Clang Static Analyzer** | `clang-analyzer-*` in [`.clang-tidy`](../.clang-tidy), every family, `optin.*` included. It follows paths through calls that no test takes. | One real bug (below). One path it couldn't rule out is now an `assert` in `OrderIndex::rehash` stating the invariant. The analyzer about doubles clang-tidy's run time. |
| **ASan options** | `detect_stack_use_after_return`, `check_initialization_order` and `strict_init_order` in the test preset | Nothing found. A two-file static-initialization-order sample is caught only with them. |

**Left off: `unsigned-integer-overflow`.** Unsigned arithmetic wraps by definition. Its 66 reports on the tests came from 10 places, and every one was wrapping on purpose:
- `OrderIndex`'s Fibonacci hash;
- the stress-test and listener checksums;
- `npos + 1` in `onStockDirectory`;
- the standard library.

Silencing them would take Clang-only attributes in our headers. CMakeLists.txt keeps the full list.

### The bug: a moved-from `OrderIndex` read an empty table

`OrderIndex` had implicit move operations. They took the slot vector but kept `size_`, `mask_` and `shift_`, so any later `find`, `erase` or `tryInsert` on the moved-from index went through an empty vector. `tryInsert` also shifted by 64 in `home()`. No caller moves an index today, but the public API allowed it.

| How it showed | |
|---|---|
| Clang Static Analyzer | `core.BitwiseShift`: right shift by 64 in `home()`, reached through `tryInsert` on a table with no slots |
| A test using the moved-from index | Failed in all three builds. gcc-release segfaulted; UBSan reported "applying non-zero offset 20240 to null pointer"; debug mode reported "out-of-bounds index 1265, but container only holds 0 elements". |

### The first fix cost 2 ns on the hot path

The first fix kept moves and made a moved-from index usable. That meant an emptiness check (`size_ == 0`) in every `find()` and `erase()`. Every take in the book calls `find()`. Nobody had measured it, so `compare.py` did: `orderbook_latency` against the original, 1,000,000 events, runs alternated, benchmark-machine thresholds and gate:

| Fix | Gated regressions against the original | `take/min` |
|-----|---------------------------------------:|-----------:|
| Moved-from index usable (emptiness check in `find()` and `erase()`) | 5 of 118 metrics with 5 runs a side, 2 with 6; all on the take path, e.g. `new_virt/depth=10/take/min` +7.7% | +1 to 2.5 ns |
| Moves deleted | 0, with 6 runs a side | ±0 |

Nothing moves an index: the book builder owns one for the whole trading day. So the moves are deleted. In a release build, `find()` is the same code as before the fix. Moving an index is a compile error, and `CompileFail.OrderIndexCannotBeMoved` proves it. One bug, three tools: the analyzer found it, a sanitizer and debug mode showed it at run time, and the benchmark gate stopped a correct fix that was slower than it had to be.

## Memory ordering: every threaded path, more schedules

ThreadSanitizer already ran the unit tests, and `TSan.CatchesRelaxedPublish` proved it catches a relaxed `publish()` ([memory_ordering.md](memory_ordering.md)). Two gaps were left:

- **Threaded code that never ran under TSan.** The two-thread harness behind `ring_latency` and `ring_study` ([`bench/RingHarness.hpp`](../bench/RingHarness.hpp)) has its own `std::atomic<bool>` handshake, and both threads write a shared result struct. The harness programs were built in the TSan preset but never run.
- **One schedule.** TSan only checks the interleavings that actually happen, and the stress test had one way of waiting: yield when the queue is full or empty.

### What runs now

[`cmake/SanitizerSmokeTests.cmake`](../cmake/SanitizerSmokeTests.cmake) adds three groups:

| Tests | Builds | What they do |
|-------|--------|--------------|
| `Stress.<queue>.<tight\|random>`, 22 | every build, label `stress` | Every SPSC hand-off runs 100,000 messages under two schedules. **Tight** busy-spins on a full or empty queue. **Random** inserts seeded random spins, yields and rare stalls of up to 2 ms between every step, on both sides. The hand-offs: `RingBuffer`, the three W03 variants, `RingV2` batched and with both indices on one cache line, and `KernelBypass`. Each queue also runs at 2 or 4 slots, where wrap-around and the full and empty edges come every few messages. Each run picks a seed and prints it, so a failure can be replayed with [`spsc_stress`](../tests/spsc_stress.cpp). |
| `TSan.CatchesRelaxedConsume` | TSan | Passes only if TSan reports the race in a queue whose `consume()` is relaxed ([`tests/RelaxedConsumeRingBuffer.hpp`](../tests/RelaxedConsumeRingBuffer.hpp)), next to the existing relaxed-`publish()` test |
| `Smoke.<program>`, 12 | sanitizer builds, label `smoke` | Every program with small inputs: `ring_latency` (paced, burst, and with a core that doesn't exist, so a failed pin must still pass), `ring_study` with and without busy work, `ring_bench`, `orderbook_latency`, `gen_itch` → `itch_test` and `itch_replay`, `MiniHFT`, `hw_bench` |

The 22 stress tests take 7 s under TSan. The `clang-tsan` CI job also repeats them 10 times, each repeat with new seeds: 220 runs, 33 s locally.

Every file with an atomic or a thread now has its threads run under TSan:

| Code | What exercises it |
|------|-------------------|
| `include/RingBuffer.hpp` | `RingBuffer.SpscStress*`, `Stress.ring*`, `Smoke.ring_latency.*`, `Smoke.ring_study*`, `Smoke.ring_bench` |
| `bench/RingVariants.hpp` | `RingVariantTest/*`, `Stress.v0*`, `Stress.v1*`, `Stress.v2*`, `Smoke.ring_study*` (all eight variants) |
| `bench/RingHarness.hpp` | `Smoke.ring_latency.*`, `Smoke.ring_study*` |
| `include/KernelBypass.hpp` | `KernelBypass.TwoThreadsDeliverEveryPacketInOrder`, `Stress.kernel-bypass.*` |
| `include/ThreadUtils.hpp` | `ThreadUtils.*`; both threads pinning at once in every ring smoke test, and both reporting a failed pin at once in `Smoke.ring_latency.unpinned` |

`include/Network.hpp` and `src/net_test.cpp` are Windows-only, so they aren't covered. No correct queue showed a problem under any schedule.

### What each piece adds: three broken queues

Three deliberately broken queues, measured with Clang 18:
- `publish()` relaxed;
- `consume()` relaxed;
- `claim()` off by one. It has the right orderings, but `>` instead of `>=` lets it accept 1,025 messages into 1,024 slots, so it races only when the queue is full.

| Broken queue | Release build, no TSan (200,000 messages, random schedule) | ThreadSanitizer |
|--------------|-----------------------------------------------------------:|-----------------|
| `publish()` relaxed | 0 of 10 runs damaged | Reported in every run, under every schedule |
| `consume()` relaxed | 0 of 10 runs damaged | 0 of 10 up to 1,024 messages in 1,024 slots, 10 of 10 from 1,025. With 2 slots: 0 of 10 at 2 messages, 10 of 10 at 3 |
| `claim()` off by one | 10 of 10 runs damaged | 5,000 messages: 4 of 20 runs with the tight schedule, **14 of 20 with the random one** |

What that shows:
- **The schedules don't help with missing orderings, but the tiny rings do.** No interleaving supplies the missing happens-before edge, so TSan reports it in every run, as soon as the racing accesses happen. For a relaxed `consume()`, that means the producer reusing a slot the consumer has read: message 1,025 in a 1,024-slot ring, message 3 in a 2-slot ring. On x86, neither bug damaged a single message without TSan.
- **The random schedule helps with bugs that depend on timing.** It took TSan's catch rate for the off-by-one from 20% to 70% at 5,000 messages. The stress test's own checks catch that bug in a release build anyway, because it really overwrites messages.

### What TSan still can't see

- **Fences.** TSan doesn't model `std::atomic_thread_fence`. Correct fence-based code would be reported as a race, and TSan couldn't tell a right fence from a wrong one. Nothing in the code uses a fence today; keep it that way, or add a different check along with the fence.
- **Ordering between atomics only.** A sleep/wake handshake that needs `seq_cst` but uses acquire/release, or a seqlock, can be wrong with no plain-memory race for TSan to report. No such protocol exists here yet. A memory-model checker would be needed for one.
- **Weakly ordered hardware.** Everything above ran on x86. Running the stress tests on ARM, where the hardware really does reorder, would be the strongest check for this area. `Tsc.hpp` has to be ported first.
- **Executions that don't happen,** as the off-by-one shows: 14 of 20 is not 20 of 20.

## Benchmark baselines

An AI can explain why a change should be faster, but it can't know until the change is measured. Before this, the latency harnesses printed tables for a person to read, and nothing compared one run against another. Now:

- the harnesses write machine-readable results;
- `bench/compare.py` judges one set of runs against another;
- the CCD matrix can save a baseline and check later commits against it;
- CI runs the same comparison between the merge-base and the head.

### Results as JSON

`ring_latency`, `ring_study`, `orderbook_latency` and `itch_replay` take `--json=FILE`, plus optional `--commit=SHA` and `--label=NAME`. The printed output is unchanged. The file records:
- the run's metadata: CPU, compiler, build type, TSC rate, timer overhead, whether every thread was pinned, and the arguments;
- a flat list of metrics with stable names.

| Program | Metrics |
|---------|---------|
| `ring_latency` | `ring_latency/send_to_receive/<stat>`, and `ring_latency/intended_to_receive/<stat>` when paced |
| `ring_study` | `ring_study/<variant>/burst/ns_per_msg`, `…/burst/throughput`, `…/paced/<stat>` |
| `orderbook_latency` | `orderbook/<old\|new\|new_tmpl\|new_virt>/depth=<d>/<take\|make>/<stat>`, and `orderbook/<new\|new_tmpl\|new_virt>/depth=<d>/mean_per_event` |
| `itch_replay` | `itch_replay/throughput`, `itch_replay/mean_per_update`, `itch_replay/<all\|add\|execute\|cancel\|delete\|replace>/<stat>` |

`<stat>` is `min`, `p50`, `p90`, `p99`, `p99.9`, `p99.99` or `max`, in ns: the columns of the printed table. `compare.py` also reads Google Benchmark's JSON (`ring_buffer_bench --benchmark_out=FILE --benchmark_out_format=json`), as `<executable>/<benchmark>/real_time` and `cpu_time`.

### compare.py

```bash
python3 bench/compare.py BASELINE... --current CURRENT... [--threshold [PATTERN=]PCT]... [--gate PATTERN]... [--markdown] [--only-changes]
```

Each side is one or more files or directories, normally repeated runs. For each metric, `compare.py` takes the median of each side's runs. A change counts only if **both** of these hold:

- it is larger than the metric's threshold (default 5%);
- the run ranges don't overlap. For a regression, the current's *best* run must be worse than the baseline's *worst*; for an improvement, the reverse.

With one run on a side the ranges say nothing about noise, so the verdict is marked unconfirmed. `compare.py` warns when the two sides differ in CPU, compiler, build type or pinning. The exit code is 0 with no regression, 1 with a regression, and 2 for bad input. 40 unit tests ([`bench/test_compare.py`](../bench/test_compare.py)) run under ctest wherever Python 3 is found.

**`--gate`, and why it exists.** The first real comparison showed the problem. A comment-only change against its own base, 3 runs a side, flagged a +288% "regression" at `p99.99` and 5 "improvements" in other tails. Every metric gets its own verdict, and `orderbook_latency` alone reports about 120 metrics. Under pure noise, all *n* runs of one side land beyond all *n* of the other 1 time in C(2*n*, *n*):

| Runs per side | 2 | 3 | 5 |
|--------------:|--:|--:|--:|
| Chance noise separates one metric in a given direction | 1 in 6 | 1 in 20 | 1 in 252 |

Across 120 metrics, a few of those will always also clear their thresholds. `--gate PATTERN` limits the exit code to the metrics that match. The others are still compared and listed, marked "slower (not gated)", for a person to read. A pattern without `/` matches the last part of a metric's name (`p50`, `mean_per_*`); one with `/` matches the whole name. `--threshold` patterns work the same way.

### On the benchmark machine

```bash
cmake --preset gcc-release && cmake --build --preset gcc-release
sudo bench/run_ccd_matrix.sh --no-build --runs=3 --save-baseline   # on the commit to compare against
# ...change, rebuild...
sudo bench/run_ccd_matrix.sh --no-build --runs=3 --check           # exit 1 on a regression
```

On Windows: `run_ccd_matrix.ps1 -Runs 3 -SaveBaseline`, then `-Check`.

- **Where results go.** Every harness run writes JSON under `build/bench-results/<timestamp>/`, labelled by core pair and benchmark. `--save-baseline` copies that set to `build/bench-baseline/`, overwriting the previous baseline.
- **What `--check` gates on:** `min`, `p50`, `mean_per_*`, `throughput` and `ns_per_msg`.
- **Thresholds:** 5% by default, 15% at `p99`, 30% at `p99.9`, 50% at `p99.99` and 100% at `max`.
- **Tails aren't gated.** They're in the table, but the OS sets them more than the code does. When a tail is what you're optimizing, read it there.

Use 3 runs or more; with 2, noise alone separates a metric 1 time in 6. Compare only builds made on the same machine with the same settings. The log header records the settings that matter ([native_linux.md](native_linux.md)).

### In CI: report only

The `bench-compare` job ([`bench/ci_compare.sh`](../bench/ci_compare.sh)) works like this:
- it builds the merge-base with `main` and the head (on `main` itself, the previous commit), with the same flags;
- it runs `orderbook_latency` and `ring_buffer_bench` from each, 5 runs a side, in ABBA order so that drift on the runner hits both sides alike;
- it writes the comparison to the job summary.

Thresholds are wider than on the benchmark machine: 10% by default, 25% at `p99`, 50% at `p99.9`, 100% at `p99.99` and 200% at `max`. The gate is `min`, `p50`, `mean_per_*`, `real_time` and `cpu_time`.

A regression raises a warning, never a failure. A GitHub runner is a shared 4-vCPU VM: comparing old and new code on the same runner can catch a large single-threaded regression, but tail latency and cross-core numbers measured there mean nothing. A warning is a reason to rerun on the benchmark machine, not a verdict.

Its first run compared only `ring_buffer_bench` (1.77 ns against 1.77 ns, within noise), because the base's `orderbook_latency` predates `--json`. Later runs compare both.

### Shown on a real regression

Setup:
- **Machine:** a 4-vCPU cloud VM (Xeon, 2.8 GHz) with two other builds running, so noisier than a CI runner.
- **Three builds**, all with the same flags:
  - the base;
  - the base plus a busy-wait of 140 TSC ticks (about 50 ns) at the top of the L3 book's `onExecuted`, which is the *take* path;
  - the base plus a comment-only change.
- **Runs:** 3 per build, alternated, compared with the CI thresholds and gate.

**The busy-wait is caught.** `compare.py` exits 1 with 18 gated regressions: `min` and `p50` of every take, for all three L3 variants at both depths (12 of 12), and every `mean_per_event` (6 of 6):

| Metric | Base | With the busy-wait | Change |
|--------|-----:|-------------------:|-------:|
| `orderbook/new/depth=10/take/min` | 25.7 ns | 83.6 ns | +225% |
| `orderbook/new/depth=10/take/p50` | 30.0 ns | 92.5 ns | +208% |
| `orderbook/new/depth=1000/take/p50` | 35.0 ns | 98.2 ns | +181% |
| `orderbook/new/depth=10/mean_per_event` | 18.5 ns | 51.7 ns | +179% |

The busy-wait shows up as about +60 ns per take: the 50 ns wait, plus roughly the cost of the TSC reads in its loop. `mean_per_event` averages takes and makes, so it moves by about half as much. The *make* path doesn't go through `onExecuted`, and none of its gated metrics moved (largest change +7.5%, within noise). Neither did the old book, which doesn't use the L3 code (largest change +2.9%).

**The comment-only change passes.**

| Comparison | Exit code without `--gate` | With `--gate` |
|------------|---------------------------:|--------------:|
| Busy-wait in `onExecuted` | 1 | **1**: 18 gated regressions |
| Comment only | 1: a false alarm at `p99.99` | **0**: the tail moves listed, not gated |

### Reproduce

```bash
cmake --preset gcc-release && cmake --build --preset gcc-release
for i in 1 2 3; do ./build/gcc-release/orderbook_latency --events=200000 --depths=10,1000 --json=base/run$i.json; done
# ...make the change, rebuild...
for i in 1 2 3; do ./build/gcc-release/orderbook_latency --events=200000 --depths=10,1000 --json=head/run$i.json; done
python3 bench/compare.py base --current head --threshold 10 --threshold p99=25 --threshold max=200 \
    --gate min --gate p50 --gate 'mean_per_*'
```

The CI job's script also runs locally: `JOBS=4 RUNS=3 bash bench/ci_compare.sh` (or `BASE_REF=<commit>` to pick the base).
