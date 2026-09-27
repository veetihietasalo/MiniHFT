# The L3 order book

W04 replaced the original `OrderBook` (each side a sorted `std::vector` of orders) with a book built from order-level ITCH events, and replayed a full NASDAQ trading day through it.

## Design

| Piece | File | Job |
|-------|------|-----|
| ITCH messages | [`ItchMessages.hpp`](../include/ItchMessages.hpp) | Layout, decode and encode for every message type. Fields are read byte by byte at their spec offsets. |
| Reader | [`ItchParser.hpp`](../include/ItchParser.hpp) | Streams length-prefixed messages from any `FILE*` (a file, or `gzip -dc` through a pipe) without copying them |
| Order index | [`OrderIndex.hpp`](../include/OrderIndex.hpp) | Order reference number → order, in O(1): open addressing, linear probing, backward-shift deletion |
| Order pool | [`ObjectPool.hpp`](../include/ObjectPool.hpp) | Orders are allocated from blocks and recycled, so no heap call per order |
| Book | [`L3OrderBook.hpp`](../include/L3OrderBook.hpp) | One instrument: price levels per side, a FIFO of orders per level |
| Builder | [`ItchBook.hpp`](../include/ItchBook.hpp) | Applies add / execute / cancel / delete / replace to the right book, and counts anything inconsistent |

Inside a book:

```text
bids_  (std::vector<PriceLevel>, ascending: best bid at the back)
   [ 149.95 | 149.96 | 149.97 | 149.99 | 150.00 ]  <- back() is the best bid
                                            |
                                  head -> order 2002 -> ... -> tail     (intrusive FIFO: time priority)
asks_  (descending: best ask at the back)
   [ 150.05 | 150.04 | 150.03 | 150.01 ]  <- back() is the best ask
```

- **Best price at the back.** Each side is sorted so the best price is `back()`. Most events happen at or near the top, so inserting or erasing a level usually shifts only a few elements, and reading the top of book is one load.
- **An intrusive FIFO per level.** Every order carries `prev` and `next` pointers, so an execution or cancel unlinks it in O(1), and orders at one price stay in time priority.
- **Orders found by reference number.** Execute, cancel, delete and replace go through `OrderIndex` (ref → order). Finding the level is a binary search by price over a contiguous array, which touches only a few cache lines.
- **No pointers into the level vector.** Levels point at orders; orders don't point at levels. So the vector may move levels when it inserts, erases or grows.
- **Replace follows the ITCH rule.** It's a *new* order: new reference number, new price and size, and it goes to the back of the queue even at the same price.

## Bugs fixed in the old code

- **The ITCH header was 2 bytes too long.** `ItchHeader` declared the timestamp as `uint64_t`, but ITCH uses 6 bytes, which made every message 2 bytes longer than the spec (an add order was 38 bytes instead of 36). It only worked because `gen_itch` wrote the same wrong layout.
- **The parser couldn't read real files.** It had no length-prefix framing, handled only `'A'`, "resynced" after unknown bytes by skipping one byte at a time, and loaded the whole file into memory.
- **`ObjectPool` tried to re-point a reference.** `current = blocks.back();` doesn't re-point `current` at the new block. It copy-assigns into the old block through a reference that `emplace_back` may just have left dangling. It had never compiled, because nothing had used that path.
- **`gen_itch` truncated the locate code.** It wrote `swap32(1)` into a 16-bit field, which stores 0.

## Old book vs new book

`orderbook_latency` puts both books through the identical event sequence at a fixed depth. A *take* removes the best order on one side: the old book adds an aggressive order and matches it, the new book receives an execution. A *make* restores the depth with a passive order at a random level. p50 in ns, including about 10 ns of timer overhead:

| Depth | take, MSVC: old → new | take, GCC 13: old → new | make, MSVC: old → new | make, GCC 13: old → new |
|------:|-----------------------|-------------------------|-----------------------|-------------------------|
| 10 | 131 → **20** | 40 → **20** | 20 → **20** | 20 → **20** |
| 100 | 191 → **20** | 91 → **20** | 50 → **20** | 30 → **20** |
| 1000 | 1,005 → **20** | 604 → **20** | 371 → **20** | 211 → **20** |

The old book's cost grows linearly with depth: inserting at, or erasing from, the front of a vector of orders shifts every order. The new book's doesn't. Its take is an index lookup plus an unlink from the top level, and its make is a binary search over levels plus an append.

## A full NASDAQ trading day

[`bench/itch_replay`](../bench/itch_replay.cpp) streamed NASDAQ's public TotalView-ITCH 5.0 sample for **30 December 2019** (`12302019.NASDAQ_ITCH50.gz` from emi.nasdaq.com, 3.5 GB compressed) through the builder. That's every symbol and every order event, with each book update timed. GCC 13 under WSL2, pinned to one core, decompressed on the fly:

```bash
gzip -dc 12302019.NASDAQ_ITCH50.gz | ./build/gcc-release/itch_replay -
```

### Correctness

| Check | Result (3 runs) |
|-------|-----------------|
| Messages | 268,744,780 (8.25 GB uncompressed); 263,241,937 of them change a book |
| Wrong message lengths, unknown types, truncated input | 0, 0, no |
| Execute / cancel / delete / replace for an order that isn't in the book | **0** |
| Execution or cancel larger than the order's remaining size | **0** |
| Live orders at the end of the day | **0** of a 1,924,078 peak: every one of the 118.6M adds was later executed, deleted or replaced |
| Crossed books during market hours | 4,738, all explained below |

**The crossed books.** On an exchange's own book, a crossed book shouldn't happen during continuous trading, because an incoming order that crosses executes immediately. So I counted every add that left a book crossed during market hours, and broke the count down by the symbol's trading state from the ITCH trading-action ('H') messages:

- **4,728 happened while the symbol was halted, paused or quote-only.** Orders keep arriving during a limit-up/limit-down pause, but nothing executes until the reopening auction. Fifteen small-cap symbols account for all of them: MKD alone had 3,157, then MBOT 529, ARDS 379 and SLGL 317.
- **The other 10 happened 184–448 µs after the symbol returned to trading.** That's MKD at 10:54, 11:21 and 11:47, and MBOT at 14:20, each while the reopening auction was clearing the orders queued during the pause.

None of them points to a book bug.

### Spot checks

Top of book as the replay reached each time (price × shares):

| Time | AAPL | MSFT | AMZN | TSLA | SPY |
|------|------|------|------|------|-----|
| 09:30:00 | 289.53 × 686 / 289.74 × 200 | 158.82 × 3 / 158.83 × 562 | 1872.00 / 1874.00 | 428.50 / 429.10 | 322.95 / 322.98 |
| 12:00:00 | 290.45 / 290.49 | 157.74 / 157.75 | 1843.34 / 1843.85 | 419.18 / 419.37 | 321.46 / 321.47 |
| 15:59:00 | 291.37 / 291.39 | 157.40 / 157.42 | 1845.36 / 1845.70 | 414.31 / 414.41 | 320.78 / 320.79 |

None of these books is crossed. During the day, spreads are 1–4¢ for AAPL, MSFT and SPY, and 10–51¢ for TSLA and the $1,800 AMZN; all are wider at the open. The last execution sits at or next to the spread: AAPL traded at 291.38 inside a 291.37 / 291.39 market at 15:59. At 10:00 AAPL's last trade was 4¢ below the bid, because the book had moved on since that trade. The prices match where these stocks traded on 30 Dec 2019, for example AAPL at about $291 and TSLA at about $414, before their later splits.

### Speed

| Book update, per order message | min | p50 | p90 | p99 | p99.9 | mean |
|--------------------------------|-----|-----|-----|-----|-------|------|
| all order messages | 10 ns | 131 ns | 284–291 ns | 495–502 ns | 757–786 ns | 168–173 ns |
| add | 10 | 122 | 182 | 284 | 462 | |
| execute | 10 | 142 | 313 | 488–495 | 677–728 | |
| cancel | 10 | 81 | 231–244 | 404–422 | 597–648 | |
| delete | 10 | 142 | 302–313 | 473–488 | 684–691 | |
| replace | 10–20 | 262–273 | 462–473 | 684–706 | 1,179–1,252 | |

End to end, including decompression in the `gzip -dc` pipe, the replay runs at **2.92–2.98 M messages/s**, so the whole day takes 90–92 s.

Each update costs about 6× more on the real day than in `orderbook_latency` (131 vs 20 ns at p50). The synthetic benchmark works on one book whose orders stay in cache. The real day spreads over thousands of books and a working set of up to 1.9M orders. The order index alone is a 128 MB table, bigger than this CPU's 96 MB L3 cache, and its hash spreads consecutive order numbers across the table on purpose. So most lookups probably miss the cache. No cache counters have confirmed that yet; `perf` needs native Linux. Replace costs about twice as much as the others because it's a delete plus an add.

## Your call: dense price array or sorted levels?

The replay counted where each book change landed, in price levels from the top of its side:

| Top | 2nd | 3rd–5th | 6th–10th | 11th–50th | 51st+ |
|----:|----:|--------:|---------:|----------:|------:|
| 44.3% | 12.7% | 13.4% | 13.3% | 13.1% | 3.3% |

The books themselves are deep. AAPL carried 3,500–4,200 bid levels and about 1,000 ask levels during the day, most of them resting orders far from the market.

- **A dense array indexed by price** makes every lookup O(1). But covering AAPL's bids from a cent to $291 in one-cent steps takes 29,100 slots × 32 bytes ≈ 0.9 MB per side, for one symbol out of thousands. A window around the current price fixes the memory, but then needs a fallback for orders outside it and re-centring when the price moves.
- **A sorted vector with the best price at the back** (what ships) is cheap exactly where 70% of the changes land, the top five levels: a short binary search, and inserts or erases that shift only a few elements. It pays a large shift only for the 3.3% of changes more than 50 levels deep, and it costs memory only for levels that exist.

For a feed handler that tracks every NASDAQ symbol, the sorted vector is the better default. A dense window would pay off for a small set of liquid symbols whose prices stay in a known band, where the O(1) lookup matters more than memory.

## Limits

- **Replay speed is measured, not optimized.** The next steps would be a cache-friendlier order index (one that keeps recently added orders together) and prefetching. They should be measured against these numbers.
- **One sample day.** A busier day, such as 30 Jan 2019 (4.8 GB), would stress the index and the deep books more.
- **Single-threaded, one core.** The WSL2 and noise caveats from W01 apply to the percentiles.
