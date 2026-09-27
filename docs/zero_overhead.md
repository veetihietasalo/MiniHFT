# Zero-overhead extension points and allocation-free hot paths

W05 asked three things of the hot path:
- a strategy should plug in without costing more than the work it does;
- the compiler should visibly inline what we think it inlines;
- nothing should touch the heap once the system is warmed up.

## Concepts instead of base classes

**`itch::ItchHandler`** ([`ItchMessages.hpp`](../include/ItchMessages.hpp)) spells out what `itch::dispatch()` needs from a handler: one `on…` method per message type. `dispatch` was already a template, so every call was resolved at compile time. The concept changes what happens when a handler is wrong. A handler missing `onTradingAction` now fails where the requirement is written, with an error naming it, instead of deep inside `dispatch`.

**`TopOfBookListener`** ([`ItchBook.hpp`](../include/ItchBook.hpp)) is the strategy hook. `BasicItchBookBuilder<Listener>` calls `listener.onTopOfBook(top)` whenever an instrument's best bid or ask changes, in price or in size. A replace is reported once, not as a delete followed by an add. The listener is a template parameter, so the call is resolved at compile time. `ItchBookBuilder` is an alias for `BasicItchBookBuilder<NoListener>`, and for `NoListener` the notification code sits behind `if constexpr` and is never generated.

Every concept is checked by `static_assert`s in [`tests/listener_test.cpp`](../tests/listener_test.cpp):
- `ItchBookBuilder` is an `ItchHandler`;
- a handler missing `onTradingAction` is not;
- `int` is not a `TopOfBookListener`.

## What the hook costs

[`orderbook_latency`](../bench/orderbook_latency.cpp) runs the L3 book three ways: with no listener, with `QuoteCounter` through the template, and with the same work behind a virtual interface (`VirtualListener` → `TopOfBookHandler*`). The handler is picked at run time, so the compiler can't see which implementation it calls.

Per-event timing can't answer this: a ~10 ns timer and histogram buckets 1.6% wide put all three at a 20 ns p50. So the whole run is timed once, with no timer inside the loop: mean ns per event over a million events, median of 5 runs, two separate runs shown.

| Depth | Compiler | No listener | Template listener | Virtual listener |
|------:|----------|------------:|------------------:|-----------------:|
| 10 | MSVC 19.51 | 10.5–11.2 | 12.6–13.2 | 14.9–15.2 |
| 10 | GCC 13 | 7.4–7.7 | 10.6–10.9 | 11.9–12.3 |
| 1000 | MSVC 19.51 | 12.8–13.7 | 14.7–14.8 | 16.9–17.1 |
| 1000 | GCC 13 | 9.6–10.0 | 13.1 | 14.4–14.5 |

- **The template listener costs 1–3 ns per event.** That's the before/after top-of-book comparison plus the listener's own counting, and it's work any strategy hook has to do.
- **The virtual call adds another 1.3–2.3 ns.** That's the indirect call, and the lost chance to inline and optimize across it.
- **Both are small next to a real day.** The median book update on the NASDAQ replay is 131 ns ([order_book.md](order_book.md)), so even the virtual hook adds about 1–2%. The full 30 Dec 2019 day, replayed with the template listener attached, confirms it:
  - **124,767,189 top-of-book changes** delivered, roughly one for every two order messages;
  - the median update was **130.9 ns, the same as without a listener**;
  - 0 unknown orders and 0 over-executions.

## What the compiler generated

The same `onDelete` for each builder type, compiled with GCC 13 `-O3 -DNDEBUG` and read:

| Builder | Instructions | Indirect calls | Listener code |
|---------|-------------:|---------------:|---------------|
| `ItchBookBuilder` (`NoListener`) | 313 | 0 | none |
| `BasicItchBookBuilder<QuoteCounter>` | 392 | 0 | inlined: the comparison and the counter, no call |
| `BasicItchBookBuilder<VirtualListener>` | 398 | **1**, `call *16(%rax)` | a call through the vtable |

All three also contain direct calls to `memmove`, `operator new` and `operator delete`. Those are `std::vector`'s paths for inserting or erasing levels and for growing, and the tests below show the allocating ones never run once the builder is warm. The roadmap suggested Compiler Explorer for this; compiling locally with `-S` shows the same thing without uploading the code.

## Zero allocations, checked in CI

[`tests/allocation_counter.cpp`](../tests/allocation_counter.cpp) replaces every form of global `operator new` and `operator delete` in the test binary, including the aligned forms that `RingBuffer` needs. Counting is per thread, and only inside `countAllocations(fn)`. [`tests/zero_alloc_test.cpp`](../tests/zero_alloc_test.cpp) then asserts:

| Hot path | Allocations |
|----------|------------:|
| `RingBuffer` push and pop, 100k times | **0** |
| `LatencyHistogram::record`, 1M values | **0** |
| `ItchBookBuilder`, a full round of adds, executions, partial cancels, replaces and deletes, after one warm-up round | **0** |
| the same with a template listener, and with a virtual one | **0** |
| the original `OrderBook`, 50 takes | **≥ 50**: `match()` returns a `std::vector<Trade>`, so every trade allocates |

A first test checks the counter itself: it must see exactly one allocation for a `make_unique<int>` and one for an over-aligned `RingBuffer`. Those tests store the pointer in a `volatile` variable. C++ lets a compiler delete a `new`/`delete` pair that nobody observes, and Clang does, which would otherwise hide the allocation.

Sanitizer runtimes define their own `operator new`, so the counting replacement can't be linked into the ThreadSanitizer build. These tests run in the MSVC and GCC CI jobs.

## Devirtualization in the Monte Carlo pricer

The other half of W05 lives in [MonteCarloCpp](https://github.com/veetihietasalo/MonteCarloCpp) ([write-up](https://github.com/veetihietasalo/MonteCarloCpp/blob/main/docs/devirtualization.md)). An `EuropeanPayoff` concept lets `price()` take any payoff type directly.
- **Measured in isolation, the payoff call gets 2.6× faster on MSVC and 12.8–14.2× on GCC.** The gain comes from vectorization and branch-free code that inlining enables, not from avoiding the call itself.
- **In the full pricer the payoff is about 1 ns of a 15–18 ns path**, so the gain there is 2–4%. The random number generator dominates.
- **`final` wasn't enough on MSVC 19.51.** It still made the indirect call.

## Your call: where is a virtual call perfectly fine?

Think it through before opening the answer.

<details>
<summary>An answer, with this project's numbers</summary>

A virtual call costs about 1–2 ns plus whatever inlining it prevents. It's fine anywhere that cost disappears against the work around it, or the path runs rarely:

1. **Configuration and start-up:** choosing strategies, feeds or venues from a config file. That runs once.
2. **Logging, metrics and diagnostics sinks,** as long as they run off the hot thread. The hot thread writes into a `RingBuffer`, and whatever reads it can be as polymorphic as it likes.
3. **Anything that already waits on a network or a disk:** session handling, reconnects, order-gateway admin messages. A microsecond of I/O makes 2 ns invisible.
4. **Even the strategy hook, if runtime flexibility is worth about 1% of a real-day update.** The virtual listener measured 1.3–2.3 ns over the template one against a 131 ns median real update. Choosing a strategy at run time, without recompiling, can be worth that. What it gives up is inlining, which matters more when the listener's own work is tiny.
5. **The Monte Carlo API for pricing a mixed portfolio of payoffs.** The payoff call is about 1 ns of a 15–18 ns path.

Where it isn't fine: tight inner loops where the call is most of the work, such as the payoff-only loop (2.6–14× slower) or per-message book code. There the inlining it prevents is worth more than the call itself.

</details>

## Reproduce

```bash
ctest --preset gcc-release -R "ZeroAlloc|TopOfBookListener"   # zero-allocation and listener tests
./build/gcc-release/orderbook_latency --depths=10,1000           # the "mean per event" lines
gzip -dc 12302019.NASDAQ_ITCH50.gz | ./build/gcc-release/itch_replay - --listener=template
```
