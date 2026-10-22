# MEMORY

Durable context for agent sessions on MiniHFT: facts that are expensive to rediscover. Newest first within each section. Facts the code or docs already state are linked, not repeated.

## Project state

- **Avellaneda-Stoikov (PR #19, 2026-10-22):** `include/Strategy.hpp` implements the paper's equations (29) and (30), and `src/main.cpp` (the `MiniHFT` program) reruns its section 3.3 simulation. The paper's Table 1 rows in README → *Avellaneda-Stoikov, rerun* were checked against the PDF; that section and the At a glance row hold the rerun's figures (seed 42, plus 20-seed means).
- **README audited claim by claim (PR #19, 2026-10-22):** everything outside Performance was checked against the code, twice. Keep new README claims to what the code shows: no "engine" or "production-grade" for the project, no "zero-copy memory mapping" or "reinterpret_cast over packed structs" for the ITCH reader, no "price-time priority" for the original `OrderBook`, and no hardware-acceleration or kernel-bypass feature for the toy models (`FpgaPipeline.hpp`, `KernelBypass.hpp`). The known weak spots are under Still open below.
- **Windows prep for native Ubuntu (PRs #16 and #18, 2026-10-21):** `bench/prepare_native_linux.ps1`, linked from `docs/native_linux.md`. Its first Windows run (the owner's PC, Windows PowerShell 5.1, 2026-10-22) crashed under `-WhatIf` in the BitLocker check: read-only calls inherited `$WhatIfPreference`, so `Get-BitLockerVolume` failed. Fixed in PR #21 (the preference goes back off at the start); everything after that check is still untested on Windows. Have the user run `-WhatIf` first and read its output with them before the real run.
  - The dry run: the user, not the agent, runs it from an administrator PowerShell, in-process, under a transcript: `Set-ExecutionPolicy -Scope Process Bypass`, `Start-Transcript $HOME\whatif.txt`, `.\bench\prepare_native_linux.ps1 -WhatIf`, `Stop-Transcript`. A non-admin agent session then reads the transcript. Redirecting with `>` would miss the "What if:" lines, which go straight to the host.
  - Ubuntu Server's installer (subiquity, 24.04.5 and main as of 2026-10-20) defaults to storage version 1, which shows no free space on a disk that already has partitions. Next to Windows on one disk, boot the stick with `subiquity-storage-version=2` before `---` on the `linux` line (after `---` it carries into the installed system). The existing ESP is then picked as `/boot/efi` by itself. Checked against the code the ISO ships and a simulation of its storage model.
- **README "At a glance" (PR #16; Avellaneda-Stoikov row PR #19):** it repeats headline numbers that the README's own Performance sections, its *Avellaneda-Stoikov, rerun* section, its feature list and `docs/ring_buffer_v2.md`, `docs/order_book.md`, `docs/zero_overhead.md` and `docs/memory_ordering.md` also state. When one changes, update every copy.
- **Platforms (since PR #10, 2026-10-07):** Windows x64 (MSVC), Linux x86-64 and Linux AArch64 (GCC 13 / Clang 18). `include/Tsc.hpp` reads the TSC on x86 and `CNTVCT_EL0` on AArch64. Other ARM toolchains, such as MSVC ARM64, still hit its `#error`.
- **Verification tools (since PR #11, 2026-10-07):** one check per area where AI-written C++ goes wrong. The map and every measurement are in [`docs/verification.md`](docs/verification.md).
- **CI matrix** (`.github/workflows/build.yml`):
  - x86: msvc-release, gcc-release, gcc-debug-stl, clang-tsan and clang-asan-ubsan.
  - `ubuntu-24.04-arm`: `gcc-release (arm64)` and `clang-tsan (arm64)`.
  - Plus fuzz, clang-tidy (now with the Clang Static Analyzer, about 3 min) and `bench-compare`, which only reports and never fails.
  - Both clang-tsan jobs run the `stress` label 10 more times with new seeds.
  - The x86 job names are unchanged, so any required checks still match.
- **Networking (2026-10-21):** a MoldUDP64 multicast feed handler, `mold_send` → `mold_recv` ([`docs/networking.md`](docs/networking.md)). POSIX only (`UdpSocket.hpp`); `Network.hpp` is still the Winsock TCP demo.
- **Test counts:** per preset in [`docs/code_quality.md`](docs/code_quality.md), in two tables (*Sanitizers* and *What CI runs on every push*). After adding tests, recount with `ctest --preset <preset> -N` and update both tables.
- **Still open:**
  - More open items are in [`docs/code_quality.md` → *Still open*](docs/code_quality.md#still-open).
  - Weak spots the README states rather than fixes: the original `OrderBook`'s priority and trade price (Features → *Original Order Book*; it stays as the L3 book's benchmark baseline), the Linux-only `gzip -dc` pipe (Features → *NASDAQ ITCH 5.0 Parser*; the cause, not in the README: `itch_replay -` reads `stdin` in text mode on Windows, with no `_setmode`), the unused `MarketSimulator.hpp` (*Project Structure*) and the two early learning docs (*Background notes*; `learning_roadmap.md` is also copied to `docs/`).
  - `hw_bench`'s FPGA timing prints 0 us at `-O3` (GCC 13.3 and Clang 18.1, 2026-10-22), and the README doesn't say so: `benchmarkFpga()` ignores `output()` and its result, so the loop is optimized away. `FpgaPipeline::output` not being `[[nodiscard]]` is on `docs/code_quality.md`'s *Still open* list.
  - `docs/GITHUB_SETUP.md` (first commit, not linked from the README, not audited) still suggests "HFT Engine", "hardware acceleration" and "reinterpret_cast over packed structs for zero-copy parsing" as resume and interview lines.
  - `Smoke.ring_study` failed once in CI (clang-asan-ubsan x86 on f349c6a, PR #18), then passed on the next commit and in 190 local runs. Most likely negative cross-core TSC deltas on the runner VM, not a queue bug. The reasoning and a proposed patch (print both counters, fail only on `outOfOrder`) are in [the PR #18 comment](https://github.com/veetihietasalo/MiniHFT/pull/18#issuecomment-6313076515). Not applied. If it fails again, apply the patch first, so the log says which counter fired.
  - `Network.hpp` is Winsock-only (W09).
  - The SPSC unit stress tests send 5M messages natively on AArch64 (`stressCount()` in `tests/SpscStress.hpp`, since PR #13). x86 and TSan keep 100k–300k. With 1024 slots, even 5M messages catch a relaxed `publish()` only now and then. The 8-slot rings catch it in most runs (1 to 1,500 damaged per 5M at the GCC rates measured on the broken `fetch_add` queue; a relaxed plain store's rate wasn't measured), so every W03 variant and `KernelBypass` (`BasicKernelBypass<8>`, since PR #17) also run with 8 slots.
  - The `Stress.*` ctests (`spsc_stress`) send a fixed 100k messages on every build, ARM included (`MINIHFT_STRESS_MESSAGES`, not `stressCount()`), under the tight and random schedules. 6 of the 24 have 2 slots (3 tight, 3 random), 2 have 4 slots (rate not measured), 2 have 8 (`kernel-bypass-tiny`) and 14 have 1024. At the 2-slot rates of the broken queue, a relaxed `publish()` damages about 1 message per 100k at the tight median (0.1 to 4 with random), and the 1024-slot ones almost never show it. Giving the six 2-slot tests 5M on native ARM would catch it in most runs; not done.

## Decisions not to undo (measured)

- **`OrderIndex` is deliberately not movable.** Its implicit moves left the source reading an empty table. Making a moved-from index usable puts an emptiness check in `find()`, which cost +1 to 2.5 ns per take in two A/B runs. `CompileFail.OrderIndexCannotBeMoved` enforces it.
- **`compare.py` gates only the central metrics** (`min`, `p50`, `mean_per_*`, throughput). With ~120 metrics and 3 runs a side, noise separates a few tail metrics in every comparison: a comment-only change "regressed" `p99.99` by +288%. Use 3 or more runs, and read tails by hand.
- **UBSan `unsigned-integer-overflow` stays off.** All 66 reports were deliberate wrapping (hashes, checksums, `npos + 1`). The list is in CMakeLists.txt.
- **libstdc++ debug mode doesn't allocate on the hot paths.** The zero-allocation tests pass under gcc-debug-stl, so they stay in that preset.

- **The MoldUDP64 `Sequencer` holds packets beyond a gap** and gives up a gap only when the caller says so (`mold_recv`: after `--gap-wait-us`, and only with every socket drained), one gap at a time. Skipping ahead at once made the B feed useless; a wall-clock wait without draining gave up gaps whose filling packet sat unread after a deschedule. Both were found by `Multicast.MoldUdp64Loopback`, the second only under CPU load. Don't simplify either back.

## Hardware facts (measured)

- **2 slots (2026-10-21, two hosts, `spsc_stress relaxed-publish-tiny`, 5 runs of 30M each):** damage in every tight and yield run, 20 of 20. 2 slots aren't reliably worse than 8: host 1 had medians of 277 and 4,797 against 15 for 8 slots, host 2 had 387 and 2,037 against 3,206. One yield run damaged 1.7% of its messages. Random stalls: damage in 9 of 10 runs of 3M. The correct 2-slot `RingBuffer` stayed at 0.
- **A relaxed `consume()` damaged nothing on N2:** 0 in 300M messages on two hosts (2 slots, tight schedule only, so not proof the core never reorders). So far only TSan has caught that bug on this hardware.
- **GitHub `ubuntu-24.04-arm` runner:** 4 vCPU Azure Cobalt 100, Arm Neoverse N2 (MIDR part `0xd49`), 64 KiB L1d per core, no SMT. BogoMIPS reads 2000, which implies a 1 GHz `CNTVCT_EL0`. That's inferred: the calibration test passed but didn't print the rate.
- **Relaxed `publish()` on N2** (full table in `docs/memory_ordering.md` → *Measured on ARM*):
  - 8-slot ring: damage in every run. 1024 slots: damage in 3 of 12 runs of 100M messages.
  - Release control: never damaged.
  - The damage rate differed by about 200× between two runner hosts (8-slot medians of 15 and 3,206 per 30M, 2026-10-21).
  - The reordering is the CPU's: GCC emits the slot `stp`s before `__aarch64_ldadd8_relax`.
- **`KernelBypass` with either handoff relaxed: 0 damaged packets on N2** (PR #17): 100M per row, 1024 and 8 descriptors, tight and yield. The codegen differs as intended (`strb` vs `stlrb`), so the core just never shows it; data and flag share 8 bytes, unlike RingBuffer's slot and index. Only TSan catches these (`TSan.CatchesRelaxedKernelBypass*`). Don't re-run this hoping for damage.
- **Busy-spinning instead of yielding did not raise the damage rate.** Ring size did. Don't retry the spin variant.

## Working in the cloud sandbox

- **Mermaid diagrams:** the Mermaid Chart MCP tool validates them. Its result is about 60 KB of SVG and lands in a file; read only `.valid` with `jq`.
- **Point review agents at the working tree, not a saved diff.** A diff goes stale with the next push, and the reviewers then refute findings that were already fixed. This 4-CPU sandbox runs 2 workflow agents at a time.
- **PowerShell 7 for syntax checks:** download `packages-microsoft-prod.deb` from https://packages.microsoft.com/config/ubuntu/24.04/, then `dpkg -i packages-microsoft-prod.deb && apt-get update && apt-get install -y powershell`, and parse with `[System.Management.Automation.Language.Parser]::ParseFile`. A clean parse in 7 doesn't prove Windows PowerShell 5.1 parses the script: 7 also accepts `&&`, `||`, `? :`, `??` and `?.`, which 5.1 rejects. Windows-only cmdlets don't run here at all.
- **GitHub archive tarballs are blocked (HTTP 403)**, so FetchContent fails. Workaround: `git clone --depth 1 -b v1.15.2 https://github.com/google/googletest` and `-b v1.9.1 …/benchmark` into the scratchpad, then configure with `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=… -DFETCHCONTENT_SOURCE_DIR_BENCHMARK=…`.
- **Local TSan needs** `apt-get install libclang-rt-18-dev`. `vm.mmap_rnd_bits` is already 28 in the sandbox.
- **Sub-agents in worktrees can't build or merge.** The auto-mode classifier denies their `cmake` runs and `git merge` as "Untrusted Code Integration".
  - Some then test against hand-written GoogleTest stand-ins, and their reports can be wrong (one said the Clang sanitizer runtimes were missing when they weren't).
  - The lead rebuilds the merged tree against the real GoogleTest before trusting any result. Benchmark-sensitive fixes get a `compare.py` A/B run.
- **Worktree agents branch from the local `origin/main`.** Run `git fetch origin main` before spawning them, or they start from a stale base.
- **AArch64 cross build:** install `g++-aarch64-linux-gnu qemu-user` and use a toolchain file with `CMAKE_CROSSCOMPILING_EMULATOR "qemu-aarch64;-L;/usr/aarch64-linux-gnu"`. qemu runs on the x86 host's memory order, so it proves the build only. Exclude `ARM.*` tests there.
- **Multicast in the sandbox:** `lo` has no MULTICAST flag; `eth0` has. Send through the default route with `IP_MULTICAST_LOOP` on (UdpSocket's default) and a receiver on the same host gets a copy. Linux grants up to twice `rmem_max` for SO_RCVBUF (8 MB here, as root).
- **Text to speech (the owner listens to lessons):** `pip install kokoro-onnx soundfile`, then from Hugging Face `onnx-community/Kokoro-82M-v1.0-ONNX`: `onnx/model.onnx` and `voices/<name>.bin` (raw float32, reshape to (-1, 1, 256), pack into an .npz). Voice `af_heart` by default. Piper's lessac voice was judged too poor. Output MP3s go in the scratchpad and to the owner with SendUserFile.
- **Weak-memory experiments need the real runner.** Put a temporary CI step on the arm64 job and read its output with the GitHub MCP `get_job_logs` (`return_content: true`). Remove the step before merge.
  - Each push lands the job on a new host, and one host's result can reverse on the next (see Hardware facts). Run on at least two: push again with the temporary step still in place.

## Conventions

- **Teach while building (the owner's standing request, 2026-10-21).** Explain each decision as you make it, and why the alternatives lost; compare the C++ with how Java does the same thing; tie it to what a quant developer needs to know. Offer an audio version of longer lessons (see the TTS note above).
- **The owner's learning plan (2026-10-22): practice code is theirs; agents grade it, never write it.** The goal is quant-developer interviews (C++, low latency). A mock interview on `RingBuffer` scored 7/15:
  - Strong: memory-ordering reasoning. They got the consume/claim handoff unaided, and release/acquire and x86 vs ARM after one hint.
  - Missed: full/empty with ever-growing indices, and the power-of-two capacity (both recalled with help). They couldn't write `claim()`/`publish()` from a blank file even when guided.
  - Diagnosis: courses and agent-written code trained recognition, not writing code from memory.
  - What helps:
    - The daily from-memory file: `Desktop\RingBufferPractice` on their Windows PC, opened by a scheduled task at 18:00, with tests and a ThreadSanitizer command in the template.
    - Before trusting a component an agent wrote, they explain it back.
    - Interviews one question at a time: a hint before the answer, scored 0–3, then they say it back from memory.
    - A re-interview about a week later, compared against the 7/15.
  - Language: they reason more precisely in English. Explain in Finnish if that helps, but have them answer interviews in English, the language firms interview in. Kokoro has no Finnish voice.
- **GCC 13 at `-O3` can raise a false `-Warray-bounds`** ("forming offset 80 is out of the bounds [0, 80]" in a `std::vector` memmove), which `-Werror` turns into a failed build. Rebuilt on GCC 13.3, it comes from `erase(begin())` in `OrderBook::match()` when a fresh `OrderBook` is built every loop step (a draft `src/main.cpp`, never committed); inserts alone don't raise it. Change the code's shape rather than add a pragma or drop the flag.
- **Numbers from papers or specs come from the document, not memory.** A recalled Avellaneda-Stoikov Table 1 (62.9 and 67.2) was wrong; the PDF says 65.0 and 68.4. Have an agent read the primary source before a figure goes into a doc.
- **README Markdown:** put template names in backticks (`` `Matrix<T>` ``). GitHub strips a bare `<T>` as an HTML tag.
- **clang-tidy's `bugprone-exception-escape` fails a `main()` that can reach a `throw` it can see** (CI's clang-tidy treats every warning as an error). `src/main.cpp` catches `std::exception` because `MarketMakerStrategy`'s constructor throws `std::invalid_argument`. A new program that calls a throwing header (`Strategy.hpp`, `Matrix.hpp`, `Statistics.hpp`'s `covariance`) needs the same. The other programs reach no `throw` and have no `try`.
- **PRs** merge as merge commits ("Merge pull request #N …"). Work on `venomity/*` branches.
- **Commits:** imperative subject; body says what was measured and why.
- **Detecting ThreadSanitizer in code:** GCC 13 sets `__SANITIZE_THREAD__`; Clang 18 doesn't, it only answers `__has_feature(thread_sanitizer)`. Check both, as `tests/SpscStress.hpp` does.
- **Windows scripts target Windows PowerShell 5.1** (`powershell -File …`, as the docs run them). The pitfalls the prep script hit are commented where they apply in `bench/prepare_native_linux.ps1`. Two that it doesn't spell out:
  - `Invoke-WebRequest` has no `-WhatIf` and ignores an inherited `$WhatIfPreference`, so it still downloads, and `-OutFile` still writes, in a dry run.
  - A failing native command doesn't throw under `$ErrorActionPreference = 'Stop'` (in 5.1 never; in 7 only with `$PSNativeCommandUseErrorActionPreference`): check `$LASTEXITCODE`.
- **Docs:** state measured numbers with the machine, compiler and count; show the failing test before the fix. x86 behaviour of shared code must stay byte-identical when porting (verify by diffing `-S` output).
