# RingBuffer v2: cache coherence, measured

W03 changed [`RingBuffer`](../include/RingBuffer.hpp) one step at a time and measured each step on its own. Every step is kept as a separate variant in [`bench/RingVariants.hpp`](../bench/RingVariants.hpp). [`bench/ring_study.cpp`](../bench/ring_study.cpp) runs them all in one process, on the same two cores, with the same settings, so each row differs from the one above it by exactly one change.

| Variant | Change from the row above |
|---------|---------------------------|
| v0 | The original queue: `fetch_add` on every publish/consume, and every call reads the other thread's index |
| v1 | `fetch_add` → `store(release)` |
| v2 | Cached indices: each side keeps a private copy of the other's index |
| v2, false sharing | v2 with both sides' fields packed onto one cache line |
| v2, batch ×N | v2 publishing `head` once per N messages |

The test is a Ryzen 9 9900X3D (one CCD enabled, SMT off), Windows 11, MSVC, both threads at high priority, producer on core 2 and consumer on core 3, a 1024-slot queue and 24-byte messages. Two measurements per variant:
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

[`RingBuffer.hpp`](../include/RingBuffer.hpp) is now v2: release stores and cached indices, with the producer's and consumer's fields on separate cache lines. There's no fixed-count batching, because a trading hot path values latency over throughput. The "RingBuffer.hpp (ships v2)" row in `ring_study` tracks the v2 variant in every run, which confirms the shipped code matches the measured one. The memory-ordering argument is unchanged ([memory_ordering.md](memory_ordering.md)).

## Your call: the new worst case

With cached indices, which cache line stops bouncing, and what is the new worst case? Think it through before opening the answer.

<details>
<summary>Answer</summary>

The *index* lines stop bouncing. The producer stops reading the consumer's `tail` line on every call, and the consumer stops reading the producer's `head` line. The worst case is a queue that keeps hitting a boundary. If it's always empty, the consumer re-reads `head` on every poll; if it's always full, the producer re-reads `tail` on every claim. That's v1's traffic again plus a branch, and the balanced burst above is exactly this case. What never stops moving is the slot data itself: every message still travels from the producer's core to the consumer's. With 24-byte messages, 2.7 slots share a cache line, so the producer writing slot *i + 1* can also steal the line the consumer is reading slot *i* from.

</details>

## Limits

- **One core cluster only.** Only one CCD is enabled on this machine, so these are same-CCD numbers. Crossing CCDs should make every coherence miss more expensive, which would widen the v1 → v2 and false-sharing gaps. That run is still to do.
- **No hardware counters.** `perf c2c` needs native Linux, and WSL2 has no access to the CPU's performance counters. The explanations above are backed by the refresh counter and by the before/after timings, not by cache-miss counts. AMD uProf on Windows could supply them.
- **The paced p99 is OS noise.** It ranged from 0.6 µs to 260 µs between identical runs, so these tables report p50 only (see W01).
- **GCC under WSL is noisier.** It can't raise thread priority there, but points the same way. Balanced burst: v0 47, v1 8.4, v2 8.1–9.9 ns/msg. Slow producer: v1 106 → v2 91 ns/msg.

## Reproduce

```bash
ring_study --high-priority                                            # balanced burst + paced
ring_study --high-priority --producer-work-ns=20 --burst=5000000 --paced=200000
ring_study --high-priority --consumer-work-ns=20 --burst=5000000 --paced=200000
ctest --preset clang-tsan -R RingVariant                              # every variant is race-free
```

On Windows the programs are in `build/msvc-release/Release/`, on Linux in `build/gcc-release/`.
