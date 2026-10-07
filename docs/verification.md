# Verification: checks for the C++ an AI gets wrong

Most of MiniHFT is written with an AI assistant. An AI is reliable in some areas of C++ and confidently wrong in others, so every area where it is unreliable gets a check that doesn't trust the author, human or AI:

| Area | How it goes wrong | What checks it | Where it runs |
|------|-------------------|----------------|---------------|
| Syntax, idioms, standard library | Rarely | Warnings as errors on three compilers, clang-tidy, and every header compiling on its own | CI ([code_quality.md](code_quality.md)) |
| Templates, concepts, overload resolution | A confident claim such as "this concept rejects X" that isn't true | [Compile-fail tests](#compile-fail-tests) | CI, every build without a sanitizer |
| Lifetimes, aliasing, undefined behaviour | Code that follows the rules on paper and breaks them in real use | AddressSanitizer + UndefinedBehaviorSanitizer, fuzzing | CI ([code_quality.md](code_quality.md)) |
| Memory ordering, lock-free code | Plausible but wrong orderings. On x86 these often compile to the same instructions as the right ones. | ThreadSanitizer, including a test that passes only if it catches a deliberately broken queue | CI ([memory_ordering.md](memory_ordering.md)) |
| Performance claims | Reasoning about caches and branches without measuring | [Benchmark baselines](#benchmark-baselines): JSON results, `compare.py`, saved baselines, and an A/B job in CI | The benchmark machine; CI only reports |

## Compile-fail tests

A `static_assert(!ItchHandler<X>)` shows what a trait answers. It doesn't show that a real call with `X` is refused. The compile-fail tests show exactly that: a call, an instantiation or an ignored `[[nodiscard]]` result must fail to compile, and fail for the expected reason.

**How a test works.** [`cmake/CompileFailTests.cmake`](../cmake/CompileFailTests.cmake) builds each file in [`tests/compile_fail/`](../tests/compile_fail/) twice:

- **As written,** in a target that isn't part of the normal build. Its ctest test builds that target. The test passes only if the build fails **and** the compiler output matches the expected reason: the concept's name, the `static_assert` message, or the warning's name. A build that succeeds can't pass, however it matches.
- **With `MINIHFT_COMPILE_FAIL_CONTROL` defined,** in the normal build. That swaps the misuse for its correct form, so the file must compile. A typo or a missing include then breaks the build, instead of passing the test by failing for the wrong reason.

The tests run one at a time, because they build inside the build tree, while the unit tests keep running in parallel. The broken targets are left out of `compile_commands.json`, so clang-tidy never sees them; it analyses the controls instead. Sanitizer builds skip the tests, since a compile error doesn't depend on the sanitizer.

**What they cover:** 27 tests, on GCC, Clang and MSVC.

| Kind | Tests | What each proves |
|------|------:|------------------|
| Concepts | 9 | `itch::dispatch` refuses a handler that is missing a method, or one that takes the wrong message type. `BasicItchBookBuilder` refuses a listener with the wrong method, or one whose `onTopOfBook` returns a value. All five `Statistics` templates refuse integer data. |
| `static_assert`s | 8 | `RingBuffer` and the three ring variants refuse a size that isn't a power of two, and a size of 0. `RingV2` refuses a batch larger than the ring. `ObjectPool` refuses an over-aligned type, and a block size of 0. |
| `[[nodiscard]]` | 10 | Under warnings-as-errors, ignoring the result of any of these calls fails the build: `pinThread`, `claim`, `peek`, `tryInsert`, `find`, `acquire`, `nicReceive`, `poll`, `ItchReader::next` and `dispatch`. |

The expected reasons name the constraint, not one compiler's wording, so the same patterns hold for all three compilers. MSVC can only be tested in CI. Its first run there passed all 27, in 32 s.

### Two bugs the tests found

On the old headers, 6 of the 27 failed on GCC:

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
