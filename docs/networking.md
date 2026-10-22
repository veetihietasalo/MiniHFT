# Networking: a MoldUDP64 multicast feed handler

NASDAQ sends ITCH over UDP multicast, wrapped in MoldUDP64: one copy of each packet, replicated by the switches to every subscriber, and usually sent twice over separate networks (the A and B feeds). Nothing is resent on that path, so the receiver has to notice what it lost. This adds that receive path, end to end:

```
gen_itch --orders=N ──> mold_send ──> UDP multicast ──> mold_recv: decode -> Sequencer -> itch::dispatch -> ItchBookBuilder
                          (feed A, and optionally B)        (gaps, duplicates, A/B arbitration)
```

| File | What it is |
|------|------------|
| [`include/MoldUdp64.hpp`](../include/MoldUdp64.hpp) | The protocol: `decode()`, `PacketWriter`, and `Sequencer`, which turns packets from one or two feeds into each message exactly once, in order. No sockets, so it is unit-tested and fuzzed. |
| [`include/UdpSocket.hpp`](../include/UdpSocket.hpp) | POSIX UDP multicast: a receiver that joins a group, and a sender. Tested on Linux only (`mold_recv` also needs `ThreadUtils.hpp`, which has no macOS port); Windows still has only the Winsock TCP code in `Network.hpp`. |
| [`src/mold_send.cpp`](../src/mold_send.cpp) | Feed simulator: replays an ITCH file as MoldUDP64, optionally on a B feed too, optionally dropping every Nth packet on feed A. |
| [`src/mold_recv.cpp`](../src/mold_recv.cpp) | Feed handler: busy-polls the sockets, builds every book, and reports what arrived, what was lost, and two latencies per packet. |

## Run it

```bash
cmake --preset gcc-release && cmake --build --preset gcc-release
cd build/gcc-release && ./gen_itch --orders=1000000 --out=feed.itch     # 2M messages, 65 MB
./mold_recv --core=3 &                                                   # joins 239.192.0.1:26400
./mold_send feed.itch --pps=100000
```

To see the B feed at work, add `--group-b=239.192.0.2` to both, and `--drop-a=3` to the sender: feed A then loses every third packet, and the receiver must still deliver everything. Loopback usually has no multicast. The sender goes out through the default route with `IP_MULTICAST_LOOP` on, so a receiver on the same host gets a copy. `--iface=<address>` picks the interface on both sides.

## Design

**The protocol knows nothing about sockets.** Lengths, counts, gaps and duplicates are all functions of the bytes, so the tests feed `Sequencer` any packet sequence in microseconds, including ones that are hard to produce on a real network, and the fuzzer feeds it millions.

**A malformed datagram is refused whole.** `decode()` checks every message block before anything is delivered. When a length is wrong, the boundaries before it can't be trusted either. Bit errors are caught earlier by the Ethernet and UDP checksums, so a malformed packet means a bug or a hostile sender, and treating it as lost lets the gap logic handle it. `sequence + count` is checked for overflow: wrapping wouldn't be undefined behaviour (it's unsigned), so UBSan wouldn't see it, but every later message would then look like a duplicate.

**A gap is waited for, then given up.**
- A packet that arrives beyond the next expected number is held, not delivered: the other feed, or a late packet, may still fill the gap. When it does, the held packets follow in order.
- `mold_recv` gives up on the oldest gap after `--gap-wait-us` (default 1000), counts its messages as lost, and continues. A production handler would also request the range from a retransmission server.
- A gap is given up only after a poll of every socket came back empty, and only the oldest one at a time (the packets beyond a later gap may have only just arrived).
- At most 4096 packets are held; beyond that, the oldest gap is given up without waiting.

**One sequencer serves both feeds.** A copy of a message already delivered is dropped as a duplicate, whichever feed it came from. That's A/B arbitration, retransmission handling and stale-packet handling in one rule. Heartbeats carry the next sequence number, so they reveal a loss at the tail of the feed even when nothing else follows.

**The socket.** It covers four classic multicast mistakes:
- The receiver binds to the group's address. Bound to `0.0.0.0:port`, it would also get every other group any process on the host joined on that port.
- `IP_ADD_MEMBERSHIP` is what makes the host receive the group; binding alone receives nothing.
- The receive buffer is requested large and the granted size printed: when the reader falls behind, the kernel drops datagrams silently once it's full.
- `SO_TIMESTAMPNS` gives the kernel's receive time, which `mold_recv` compares with the time it reads the datagram.

## What the tests found before merge

The first version passed its unit tests. The tests that ran it for real didn't pass it:

| Bug | How it showed | Fix |
|-----|---------------|-----|
| **The B feed couldn't fill a gap.** The sequencer skipped ahead the moment a packet arrived beyond the next expected number. The receiver polls feed A first, so when A had dropped packet *k* and both sockets had data, it read A's *k+1* before B's copy of *k*, declared *k* lost, and then threw B's copy away as stale. | The loopback test's `b-fills` scenario: 372 messages lost in 9 gaps although B carried every packet | Packets beyond a gap are held until it fills or is given up |
| **A gap was given up while the packet that fills it waited unread.** The wait was measured in wall-clock time. With the CPU loaded, the scheduler paused the receiver for milliseconds; on resuming, it saw the wait expired, while B's copy had arrived in the socket queue meanwhile. | `b-fills` again, only with all four CPUs busy: 126 messages lost on the first run; 0 failures in 25 runs once fixed | Give up only once every socket has been drained |
| **Lost messages were undercounted at shutdown.** With two gaps open, giving up closed only the oldest, and `mold_recv` gave up once at the end, so the second gap's messages weren't counted. | `mold_fuzz`, after 580 inputs: its invariant "giving up closes the gap" failed on an input with two gaps | The contract now says *the oldest* gap; `mold_recv` gives up until none is open; the input is kept in [`tests/fuzz/mold_corpus/`](../tests/fuzz/mold_corpus/) |

## Measured on loopback

The machine is a 4-vCPU cloud VM, with the receiver pinned to one core and the sender to another. The feed is `gen_itch --orders=1000000`: 1,999,920 messages in 47,621 packets of up to 1,400 bytes. These are shapes, not results; the numbers that count belong on the benchmark machine.

| Receive buffer (granted) | Sender | Lost | Book checks | Kernel → app p50 | p99 |
|-------------------------:|--------|------|-------------|------:|------:|
| 8 MB | unpaced, 0.26 s | 0 | clean | 20 µs | **3.7 ms** |
| 416 KB (the Linux default, doubled) | unpaced | **5,502 messages in 14 gaps** | **1,368 unknown references** | 1.9 µs | 0.43 ms |
| 416 KB | 100,000 packets/s | 0 | clean | 13 µs | 0.23 ms |

- **A big buffer turns loss into latency.** The 8 MB buffer absorbed the whole 65 MB burst, and packets waited up to 4 ms in it. For a trading system a price that old is close to worthless.
- **A small buffer turns it into loss, and loss corrupts the book.** Each lost packet took its Add messages with it, so later executions and deletes referred to orders the book never saw. The gap counter is what tells you the book is wrong.
- **Processing** (decode, sequencing and book updates) took about 2.9 µs per packet at the median, about 69 ns per message, in line with the book benchmarks.

On the benchmark machine, the same two programs pinned to cores in one CCD and then across CCDs would give the real numbers. `--rcvbuf` and `--pps` reproduce the rows above.

## Tests

- **Unit tests:** 17 in [`tests/mold_udp64_test.cpp`](../tests/mold_udp64_test.cpp): the round trip, every malformed case, and the sequencer's ordering, A/B arbitration, holding, giving up, heartbeats, end of session and sessions. Breaking any of three decisions on purpose (the overflow check, heartbeats moving the sequence, dropping duplicates) fails exactly the test written for it.
- **`Multicast.MoldUdp64Loopback`:** the real programs over real multicast, in three scenarios. A clean feed must arrive whole; feed A dropping every third packet must be reported as lost; the same drops with a B feed must arrive whole. It runs on every POSIX build, the sanitizer builds included. ([`tests/mold_loopback.sh`](../tests/mold_loopback.sh))
- **`mold_fuzz`:** sequences of arbitrary datagrams through the decoder and sequencer into a book builder. It checks that every message is delivered exactly once, in order, and that giving up always makes progress. 8.7 million inputs in 120 s found nothing after the fix; CI runs it for 60 s on every push.

## Not done yet

- **Retransmission requests:** to a MoldUDP64 request server, so a gap can be filled instead of given up.
- **Windows:** porting `UdpSocket` to Winsock, which would also retire the TCP code in `Network.hpp`.
- **Batching receives with `recvmmsg`:** one system call for many datagrams.
- **NIC hardware timestamps and kernel bypass:** the next steps down in latency (see the lesson on where the time goes).
