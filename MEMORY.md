# MEMORY

Durable context for agent sessions on MiniHFT: facts that are expensive to rediscover. Newest first within each section. Facts the code or docs already state are linked, not repeated.

## Project state

- **Platforms (since PR #10, 2026-10-07):** Windows x64 (MSVC), Linux x86-64 and Linux AArch64 (GCC 13 / Clang 18). `include/Tsc.hpp` reads the TSC on x86 and `CNTVCT_EL0` on AArch64. Other ARM toolchains, such as MSVC ARM64, still hit its `#error`.
- **Verification tools (since PR #11, 2026-10-07):** one check per area where AI-written C++ goes wrong. The map and every measurement are in [`docs/verification.md`](docs/verification.md).
- **CI matrix** (`.github/workflows/build.yml`):
  - x86: msvc-release, gcc-release, gcc-debug-stl, clang-tsan and clang-asan-ubsan.
  - `ubuntu-24.04-arm`: `gcc-release (arm64)` and `clang-tsan (arm64)`.
  - Plus fuzz, clang-tidy (now with the Clang Static Analyzer, about 3 min) and `bench-compare`, which only reports and never fails.
  - Both clang-tsan jobs run the `stress` label 10 more times with new seeds.
  - The x86 job names are unchanged, so any required checks still match.
- **Test counts:**
  - 137 on gcc-release x86, gcc-debug-stl and MSVC: 84 unit, 28 `CompileFail.*`, 24 `Stress.*`, and `BenchTools.ComparePy`.
  - 138 on gcc-release arm64, which adds `ARM.RelaxedPublishDamagesMessages`.
  - Sanitizer builds skip the compile-fail tests and add 12 `Smoke.*`: 117 on clang-tsan (both architectures, with the three `TSan.CatchesRelaxed*` from spsc_stress), 113 on clang-asan-ubsan.
- **Still open:**
  - `Network.hpp` is Winsock-only (W09).
  - The SPSC unit stress tests send 5M messages natively on AArch64 (`stressCount()` in `tests/SpscStress.hpp`, since PR #13). x86 and TSan keep 100k–300k. With 1024 slots, even 5M messages catch a relaxed `publish()` only now and then; only the 8-slot rings catch it reliably, so every W03 variant and `KernelBypass` (`BasicKernelBypass<8>`, since PR #17) also run with 8 slots.
  - The `Stress.*` ctests (`spsc_stress`) send a fixed 100k messages on every build, ARM included (`MINIHFT_STRESS_MESSAGES`, not `stressCount()`). How often their 2-slot queues show a relaxed `publish()` on N2 is unmeasured: `spsc_stress relaxed-publish-tiny tight 30000000` in a temporary arm64 CI step would tell.

## Decisions not to undo (measured)

- **`OrderIndex` is deliberately not movable.** Its implicit moves left the source reading an empty table. Making a moved-from index usable puts an emptiness check in `find()`, which cost +1 to 2.5 ns per take in two A/B runs. `CompileFail.OrderIndexCannotBeMoved` enforces it.
- **`compare.py` gates only the central metrics** (`min`, `p50`, `mean_per_*`, throughput). With ~120 metrics and 3 runs a side, noise separates a few tail metrics in every comparison: a comment-only change "regressed" `p99.99` by +288%. Use 3 or more runs, and read tails by hand.
- **UBSan `unsigned-integer-overflow` stays off.** All 66 reports were deliberate wrapping (hashes, checksums, `npos + 1`). The list is in CMakeLists.txt.
- **libstdc++ debug mode doesn't allocate on the hot paths.** The zero-allocation tests pass under gcc-debug-stl, so they stay in that preset.

## Hardware facts (measured)

- **GitHub `ubuntu-24.04-arm` runner:** 4 vCPU Azure Cobalt 100, Arm Neoverse N2 (MIDR part `0xd49`), 64 KiB L1d per core, no SMT. BogoMIPS reads 2000, which implies a 1 GHz `CNTVCT_EL0`. That's inferred: the calibration test passed but didn't print the rate.
- **Relaxed `publish()` on N2** (full table in `docs/memory_ordering.md` → *Measured on ARM*):
  - 8-slot ring: damage in every run. 1024 slots: damage in 3 of 12 runs of 100M messages.
  - Release control: never damaged.
  - The damage rate varies roughly 10× between runner hosts.
  - The reordering is the CPU's: GCC emits the slot `stp`s before `__aarch64_ldadd8_relax`.
- **`KernelBypass` with either handoff relaxed: 0 damaged packets on N2** (PR #17): 100M per row, 1024 and 8 descriptors, tight and yield. The codegen differs as intended (`strb` vs `stlrb`), so the core just never shows it; data and flag share 8 bytes, unlike RingBuffer's slot and index. Only TSan catches these (`TSan.CatchesRelaxedKernelBypass*`). Don't re-run this hoping for damage.
- **Busy-spinning instead of yielding did not raise the damage rate.** Ring size did. Don't retry the spin variant.

## Working in the cloud sandbox

- **GitHub archive tarballs are blocked (HTTP 403)**, so FetchContent fails. Workaround: `git clone --depth 1 -b v1.15.2 https://github.com/google/googletest` and `-b v1.9.1 …/benchmark` into the scratchpad, then configure with `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=… -DFETCHCONTENT_SOURCE_DIR_BENCHMARK=…`.
- **Local TSan needs** `apt-get install libclang-rt-18-dev`. `vm.mmap_rnd_bits` is already 28 in the sandbox.
- **Sub-agents in worktrees can't build or merge.** The auto-mode classifier denies their `cmake` runs and `git merge` as "Untrusted Code Integration".
  - Some then test against hand-written GoogleTest stand-ins, and their reports can be wrong (one said the Clang sanitizer runtimes were missing when they weren't).
  - The lead rebuilds the merged tree against the real GoogleTest before trusting any result. Benchmark-sensitive fixes get a `compare.py` A/B run.
- **Worktree agents branch from the local `origin/main`.** Run `git fetch origin main` before spawning them, or they start from a stale base.
- **AArch64 cross build:** install `g++-aarch64-linux-gnu qemu-user` and use a toolchain file with `CMAKE_CROSSCOMPILING_EMULATOR "qemu-aarch64;-L;/usr/aarch64-linux-gnu"`. qemu runs on the x86 host's memory order, so it proves the build only. Exclude `ARM.*` tests there.
- **Weak-memory experiments need the real runner.** Put a temporary CI step on the arm64 job and read its output with the GitHub MCP `get_job_logs` (`return_content: true`). Remove the step before merge.

## Conventions

- **PRs** merge as merge commits ("Merge pull request #N …"). Work on `venomity/*` branches.
- **Commits:** imperative subject; body says what was measured and why.
- **Detecting ThreadSanitizer in code:** GCC 13 sets `__SANITIZE_THREAD__`; Clang 18 doesn't, it only answers `__has_feature(thread_sanitizer)`. Check both, as `tests/SpscStress.hpp` does.
- **Docs:** state measured numbers with the machine, compiler and count; show the failing test before the fix. x86 behaviour of shared code must stay byte-identical when porting (verify by diffing `-S` output).
