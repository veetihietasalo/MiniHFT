# RingBuffer v2: cache coherence, measured

W03 changed [`RingBuffer`](../include/RingBuffer.hpp) one step at a time and measured each step on its own. Every step is kept as a separate variant in [`bench/RingVariants.hpp`](../bench/RingVariants.hpp). [`bench/ring_study.cpp`](../bench/ring_study.cpp) runs them all in one process, on the same two cores, with the same settings, so each row differs from the one above it by exactly one change.

| Variant | Change from the row above |
|---------|---------------------------|
| v0 | The original queue: `fetch_add` on every publish/consume, and every call reads the other thread's index |
| v1 | `fetch_add` → `store(release)` |
| v2 | Cached indices: each side keeps a private copy of the other's index |
| v2, false sharing | v2 with both sides' fields packed onto one cache line |
| v2, batch ×N | v2 publishing `head` once per N messages |

The test is a Ryzen 9 9900X3D (one CCD enabled, SMT off; both CCDs under [Across CCDs](#across-ccds)), Windows 11, MSVC, both threads at high priority, producer on core 2 and consumer on core 3, a 1024-slot queue and 24-byte messages. Two measurements per variant:
- **burst:** messages sent back-to-back; time per message is the median of 5 runs.
- **paced:** one message every 1 µs; one-way latency percentiles.

Every variant passes the checksum stress test, under ThreadSanitizer too ([`tests/ring_variants_test.cpp`](../tests/ring_variants_test.cpp)).

## Results

### Both threads as fast as possible

| Variant | Burst ns/msg (run 1 / run 2) | Throughput | Paced p50 | What it shows |
|---------|------------------------------|------------|-----------|---------------|
| v0 `fetch_add` | 39.2 / 37.9 | 26 M msg/s | 50–70 ns | A locked read-modify-write on every publish and consume |
| **v1 `store(release)`** | **6.4 / 6.2** | **160 M msg/s** | **50 ns** | **6× faster**: a single-writer index needs no lock |
| v2 cached indices | 7.0 / 6.5 | 150 M msg/s | 50 ns | No gain in this regime (explained below) |
| v2 false sharing | 13.4 / 13.1 | 75 M msg/s | 60 ns | 2× slower than v2 |

### One side doing work

Real threads do work per message: a feed handler parses a packet before publishing it, and a strategy reacts after reading one. With 20 ns of busy work on one side, the queue settles near empty (slow producer) or near full (slow consumer):

| Variant | Slow producer, ns/msg (run 1 / run 2) | Slow consumer, ns/msg (run 1 / run 2) |
|---------|---------------------------------------|---------------------------------------|
| v0 `fetch_add` | 57.7 / 57.0 | 59.1 / 63.2 |
| v1 `store(release)` | 42.3 / 42.7 | 40.8 / 42.4 |
| **v2 cached indices** | **36.3 / 36.3** | **34.3 / 34.5** |
| v2 false sharing | 40.4 / 40.4 | 38.4 / 39.5 |

These times include the 20 ns of work, so compare rows rather than absolute values. Cached indices save about 6–8 ns per message here, 14–19% of the total.

### Batching: throughput vs latency

| `head` published every | Burst ns/msg | Throughput | Paced p50 |
|-------------------------|--------------|------------|-----------|
| 1 message (v2) | 7.0 | 144 M msg/s | 50 ns |
| 4 messages | 6.1 | 164 M msg/s | 2.1 µs |
| 16 messages | 3.95 | 253 M msg/s | 7.3 µs |
| 64 messages | 2.4 | 411 M msg/s | 32 µs |

## Why each change does what it does

- **`fetch_add` → `store`.** `fetch_add` compiles to `lock xadd`, which takes exclusive ownership of the cache line and acts as a full barrier: the core's store buffer must drain before it completes. `head` and `tail` each have a single writer, so a plain `mov` with release semantics is enough. The store buffer absorbs its latency and the core moves on. This change dominates everything else here.
- **Cached indices.** In v1, every `claim()` reads `tail` and every `peek()` reads `head`. Each read pulls the other thread's cache line into a shared state, so that thread's next index store must first invalidate the copy: a coherence round-trip per message on each side. v2 reads the other side's index only when its private copy says the queue is full or empty.

  That only helps if the queue is sometimes neither full nor empty. A counter in the balanced burst showed it never is: the producer re-read `tail` 2.0–2.3 times per message and the consumer re-read `head` 3.4–4.6 times. Both sides sit on a boundary constantly, which is v1's behaviour plus a branch. Give one side work and the queue settles: the thread with work stops re-reading, and the 6–8 ns gain appears.
- **False sharing.** Packing both sides' fields onto one line means every index write by either thread invalidates the other's copy of the line, even though they never touch the same variable. The line bounces between the cores twice per message, which costs 6.5 ns per message on the balanced burst (13.4 vs 7.0).
- **Batching.** Publishing `head` once per N messages spreads the producer's store and the consumer's cache miss on `head` over N messages: 2.5–2.9× the throughput at N = 64. But at one message per µs, a message waits until N−1 more have arrived, so the median latency grows to about N/2 µs. On a latency-critical path, batch *what is already there* instead of a fixed count: publish whenever the producer has no more input waiting. A quiet market then gets single-message latency, and a burst gets batched throughput.

## What ships

[`RingBuffer.hpp`](../include/RingBuffer.hpp) is now v2: release stores and cached indices, with the producer's and consumer's fields on separate cache lines. There's no fixed-count batching, because a trading hot path values latency over throughput. The "RingBuffer.hpp (ships v2)" row in `ring_study` tracks the v2 variant in every regime with work on one side, which confirms the shipped code matches the measured one. The one exception is the balanced burst inside one CCD on 12 cores (see [below](#the-balanced-burst-is-unstable)). The memory-ordering argument is unchanged ([memory_ordering.md](memory_ordering.md)).

## Your call: the new worst case

With cached indices, which cache line stops bouncing, and what is the new worst case? Think it through before opening the answer.

<details>
<summary>Answer</summary>

The *index* lines stop bouncing. The producer stops reading the consumer's `tail` line on every call, and the consumer stops reading the producer's `head` line. The worst case is a queue that keeps hitting a boundary. If it's always empty, the consumer re-reads `head` on every poll; if it's always full, the producer re-reads `tail` on every claim. That's v1's traffic again plus a branch, and the balanced burst above is exactly this case. What never stops moving is the slot data itself: every message still travels from the producer's core to the consumer's. With 24-byte messages, 2.7 slots share a cache line, so the producer writing slot *i + 1* can also steal the line the consumer is reading slot *i* from.

</details>

## Across CCDs

Everything above ran with one CCD enabled. With both enabled (12 cores, SMT on), [`bench/run_ccd_matrix.ps1`](../bench/run_ccd_matrix.ps1) ran every regime on three pairs of physical cores: inside CCD0 (the 96 MB V-cache die), inside CCD1 (32 MB), and from CCD0 to CCD1. It made two passes over the three pairs, alternating between them. The CCDs don't share an L3. A cache line that moves between them goes out through Infinity Fabric and the I/O die.

### One message every 1 µs

`ring_latency`, send → receive in ns. Each cell is the range over the two runs.

| Pair | p50 | p90 | p99 | p99.9 | max |
|------|----:|----:|----:|------:|----:|
| inside CCD0 (cores 4 → 6) | 50 | 61 | 71 | 845–1,005 | 17,200–19,700 |
| inside CCD1 (cores 16 → 18) | 49 | 60 | 70 | 1,195–1,253 | 43,700–72,600 |
| **CCD0 → CCD1 (cores 4 → 16)** | **171–180** | **262–291** | **320–331** | 1,705–1,923 | 19,200–80,900 |

- **Crossing adds about 125 ns per hop, 3.5× the same-CCD hop.** The minimum doubles, from 30 to 60 ns.
- **Inside either CCD the hop is the same 50 ns.** A 24 KB queue fits in L2, so the size of the L3 never comes into it.
- **CCD1's far tail is worse in both runs:** p99.99 is 14 µs there, against 2.7–3.5 µs on CCD0. A likely cause is that Windows puts background work on CCD1 first: AMD's driver prefers the higher-clocked die for anything that isn't a game. This hasn't been checked.

### Throughput by variant

Burst, ns per message, run 1 / run 2:

| Variant | Inside CCD0 | Inside CCD1 | CCD0 → CCD1 |
|---------|-------------|-------------|-------------|
| v0 `fetch_add` | 37.1 / 36.4 | 36.5 / 36.8 | 142.8 / 134.2 |
| v1 `store(release)` | 6.9 / 6.3 | 5.7 / 5.9 | **15.6 / 20.2** |
| v2 cached indices | 6.5 / 5.0 | 5.3 / 4.9 | 25.3 / 25.5 |
| v2 false sharing | 12.9 / 9.0 | 9.3 / 10.1 | 30.4 / 30.1 |
| v2 batch ×16 | 3.6 / 2.8 | 3.1 / 2.8 | 6.8 / 7.0 |
| v2 batch ×64 | 2.5 / 2.5 | 2.4 / 2.3 | 5.9 / 6.0 |

With 20 ns of busy work on one side. CCD1 is left out because it matches CCD0 to within 3 ns in every row:

| Variant | Slow producer, CCD0 | Slow producer, across | Slow consumer, CCD0 | Slow consumer, across |
|---------|---------------------|-----------------------|---------------------|-----------------------|
| v0 `fetch_add` | 54.9 / 55.6 | 165.5 / 165.3 | 56.0 / 56.9 | 144.0 / 145.0 |
| v1 `store(release)` | 39.2 / 40.2 | 68.2 / 68.2 | 39.3 / 39.1 | 63.7 / 61.5 |
| **v2 cached indices** | **35.2 / 35.2** | **57.2 / 57.2** | **32.3 / 32.8** | **45.4 / 45.0** |
| v2 false sharing | 38.3 / 38.3 | 59.1 / 59.1 | 36.9 / 37.1 | 67.7 / 67.7 |

Paced across CCDs, the median is 171–180 ns for v1 and v2, 240–251 ns for false sharing and 251–262 ns for v0.

### What crossing changes

- **Once the queue settles, the predicted widening shows up.** With work on one side, cached indices save 11 ns with a slow producer and 16–18 ns with a slow consumer across CCDs. Inside one CCD they save 4–5 and 6–7 ns. Every re-read of the other side's index that they avoid would now be a miss to the other die.
- **False sharing costs most with a slow consumer across CCDs: 22 ns, a 50 % slowdown.** Inside one CCD the same regime loses 4–5 ns. With a slow producer it costs only 2 ns across, so where the packed line hurts depends on which side is waiting.
- **`fetch_add` costs about 4× more across CCDs** (134–143 ns, against 36–37 inside one). That's a locked read-modify-write on a line the other die owns. v1's plain store is 7–9× faster there.
- **In the balanced burst, cached indices lose across CCDs:** v1 runs at 15.6–20.2 ns and v2 at 25.3–25.5. Inside one CCD the two tie. This isn't explained yet. One guess: v1's consumer pays a cross-die read of `head` on every call, which keeps it further behind the producer, so each slot line it reads is already complete. v2's consumer catches up and reads lines the producer is still filling, which is the slot-sharing worst case described under "Your call" above. A refresh counter like the one used for W03 would settle it.
- **Batching pays off more across CCDs.** ×16 gives 3.7× v2's throughput there (7 vs 25 ns), against 1.8× inside one CCD, and the latency cost is the same (a paced median of 8.2–8.5 µs).

### The balanced burst is unstable

Inside one CCD, the "RingBuffer.hpp (ships v2)" row measured 1.7–5.3 ns in the balanced burst, against 4.9–6.5 ns for the v2 variant. In every work regime, and across CCDs, the two still match to within 1.2 ns. The consumer code is identical. The producer differs only in keeping a separate private write index. A balanced burst balances between the queue running empty and running full, and a small difference in generated code can tip it either way. False sharing moved by 4 ns between the two runs as well. So read balanced-burst figures inside one CCD as ±4 ns, and use the work regimes to compare designs.

## Limits

- **SMT was on for the cross-CCD runs.** The script never pins a thread to the sibling of a pinned core, but Windows was free to schedule other work there. A run with SMT off and both CCDs on would separate that effect from the effect of having 12 cores.
- **No hardware counters.** `perf c2c` needs native Linux, and WSL2 has no access to the CPU's performance counters. The explanations above are backed by the refresh counter and by the before/after timings, not by cache-miss counts. AMD uProf on Windows could supply them.
- **On 6 cores the paced p99 is OS noise.** It ranged from 0.6 µs to 260 µs between identical runs, so the one-CCD tables report p50 only (see W01). With 12 cores it settled at 70–100 ns inside a CCD and 320–400 ns across, for every variant.
- **GCC under WSL is noisier.** It can't raise thread priority there, but points the same way. Balanced burst: v0 47, v1 8.4, v2 8.1–9.9 ns/msg. Slow producer: v1 106 → v2 91 ns/msg.

## Reproduce

```bash
ring_study --high-priority                                            # balanced burst + paced
ring_study --high-priority --producer-work-ns=20 --burst=5000000 --paced=200000
ring_study --high-priority --consumer-work-ns=20 --burst=5000000 --paced=200000
ctest --preset clang-tsan -R RingVariant                              # every variant is race-free
```

```powershell
powershell -ExecutionPolicy Bypass -File bench\run_ccd_matrix.ps1    # all of the above inside each CCD and across, twice (Windows)
```

On Windows the programs are in `build/msvc-release/Release/`, on Linux in `build/gcc-release/`.
