# tick-to-trade

From market data in to an order on the wire, in C++20 for Linux, written from public
specifications and built from three repos: Nasdaq TotalView-ITCH 5.0 in through
[itch-feed-handler](https://github.com/Mukachi/itch-feed-handler), a strategy and pre-trade risk
checks taken from the text of EU RTS 6 Article 15 and SEC Rule 15c3-5, and FIX 4.4 out through
[fix-order-gateway](https://github.com/Mukachi/fix-order-gateway), the threads joined by
[lockfree-ipc](https://github.com/Mukachi/lockfree-ipc)'s queues and measured on its harness.
Tick to trade is measured open loop, from the moment our sender meant to send the market data
message that caused an order to the moment the venue read the order, and split into stages.
Every component is checked against a simple reference implementation over a whole real trading
day. Where something is unexplained, the notes say so.

## Results

Ranges over three runs on a dedicated, isolated machine. Full tables, every point and stage,
the components' own figures beside them, the method and every run's conditions:
[RESULTS.md](RESULTS.md). These blocks are generated from the committed JSON in `results/` by
`python run.py report`; the test suite fails if they disagree.

Tick to trade, the default arrangement and journal, over loopback and over a 10GBASE-T card
([note 005](docs/design/005-measurement.md)):

<!-- results:t2t-summary -->
| Path | Point | Orders timed | Caused by an answer | p50 | p99 | p99.9 | max | Kernel drops | Sender late, max |
|---|---|---|---|---|---|---|---|---|---|
| loopback | A, journal C, 25k | 699,579–701,851 | 19% | 22.0 µs–22.2 µs | 66.4 µs–68.1 µs | 84.4 µs–86.5 µs | 119.3 µs–121.6 µs | 0 | 3.5–17.5 µs |
| card | A, journal C, 25k | 623,096–629,219 | 23% | 53.9 µs–55.6 µs | 100.1 µs–105.6 µs | 116.2 µs–122.9 µs | 162.5 µs–192.2 µs | 0 | 3,117–3,405 |

From our sender's intended send time of the ITCH message that caused an order to the venue's read of the order, open loop, 25,000 packets a second, arrangement A, journal C; ns, range over runs. Every point, its stages and the method: [RESULTS.md](RESULTS.md).
<!-- /results -->

The pre-trade risk checks' cost, against no checks and against their reference implementation
([note 003](docs/design/003-pre-trade-risk.md)):

<!-- results:risk-summary -->
| Case | M events/s | Book event p50 | p99 | p99.9 | Check p50 | p99 | Answer p50 | p99 | Intents | Sent | Refused |
|---|---|---|---|---|---|---|---|---|---|---|---|
| no checks | 12.26–12.27 | 23 | 105–106 | 247–249 | 21 | 29 | 22 | 78–79 | 935,763 | 935,763 | 0 |
| checks | 10.85–10.88 | 34 | 118–119 | 260–262 | 25 | 40–41 | 32 | 128–129 | 1,109,961 | 60,734 | 1,049,227 |
| reference | 1.39–1.45 | 79 | 3,724–4,361 | 14.8 µs–21.8 µs | 1,789–1,829 | 14.4 µs–20.7 µs | 594–649 | 16.3 µs–24.2 µs | 1,109,961 | 60,734 | 1,049,227 |

The strategy's thread on one CPU over the sample day's first 50,000,000 messages' book events for 100 instruments, with no checks, the risk checks, or their reference implementation; ns, range over runs. The method: [RESULTS.md](RESULTS.md).
<!-- /results -->

What the strategy's view of the market costs the feed handler, against its own publisher
([note 002](docs/design/002-book-events-strategy.md)):

<!-- results:sink-summary -->
| Case | M messages/s | p50 | p99 | p99.9 | max | Book events |
|---|---|---|---|---|---|---|
| no sink | 4.28–4.37 | 197–198 | 618–638 | 891–955 | 4.0–18.5 µs | – |
| publisher (table only) | 3.14–3.18 | 267–269 | 751–771 | 1,050–1,097 | 4.0–17.6 µs | – |
| sink, 1 instrument | 4.11–4.17 | 204–205 | 631–646 | 906–962 | 6,559–7,730 | 392,472 |
| sink, 10 instruments | 4.04–4.10 | 207–208 | 637–653 | 914–966 | 3.8–17.9 µs | 1,722,283 |
| sink, 100 instruments | 3.88–3.93 | 216–218 | 649–670 | 921–973 | 4.5–17.9 µs | 4,717,340 |

The sample day's first 50,000,000 messages through the feed handler's handler on one CPU, with no sink, its publisher, or the strategy's sink; ns a message, range over runs. The method: [RESULTS.md](RESULTS.md).
<!-- /results -->

## What is here

| Component | Note | Header |
|---|---|---|
| Book events: the feed handler's sink, and an adapter for its publisher's updates | [002](docs/design/002-book-events-strategy.md) | `sink.hpp`, `adapter.hpp`, `events.hpp` |
| Event ring between the feed and the strategy (the newest event per instrument kept aside when full) | [002](docs/design/002-book-events-strategy.md) | `event_ring.hpp` |
| Strategy: one buy order per instrument resting at the best bid, following it | [002](docs/design/002-book-events-strategy.md) | `strategy.hpp` |
| Pre-trade risk: RTS 6 Art. 15 order checks, execution throttle, 15c3-5 exposure, kill switch | [003](docs/design/003-pre-trade-risk.md) | `risk.hpp`, `limits.hpp` |
| Orders and answers: FIX bodies written for the gateway, its reports read back | [004](docs/design/004-program.md) | `orders.hpp`, `trader.hpp` |
| Configuration and the control and monitoring block | [004](docs/design/004-program.md) | `config.hpp`, `block.hpp` |
| Reference strategy and risk checks: the oracles and baselines | 002, 003 | `reference_*.hpp` |

Header-only, in `include/ttt/`. The program `ttt` (`apps/`) runs them in one of three
arrangements from one configuration: **A**, the default, the feed on one thread handing book
events through a ring to the strategy and risk thread, which hands orders to the gateway's
engine thread; **B**, the feed, the strategy and the risk on one thread; **C**, the strategy as a
consumer process of the feed handler's own program, `itchfh`, unchanged. The gateway journals
either to memory that survives a process crash (C, the default) or to disk with `fdatasync`,
surviving power loss (A). `ttt-ctl` engages the kill switch, re-enables or stops the program;
`ttt-monitor` reads its counters. The venue is the gateway's `order_venue`, and the market data
comes from the feed handler's own MoldUDP64 sender, both built here from their pinned sources.

In steady state the hot path allocates nothing, and a test checks its compiled call graph: no
allocation, and no system call but the reads and writes it is meant to make.

## Correctness

- **The whole day against the references.** The sample day's 423 million messages through the
  sink and the strategy, and through the risk checks, each compared with its reference
  implementation after every event: the same intents, the same refusals, the same state.
- **Every order passed every check.** Over the whole day through the programs, in each
  arrangement, the gateway's journal holds exactly the orders the risk checks passed, in order;
  each check refuses the orders built to break it, counted by reason.
- **Failures.** The kill switch (no new order, every open order cancelled), a dropped session
  (nothing sent while not logged on) and a lost feed (no order on an unreliable book until it is
  recovered from a snapshot) are each tested through the programs; with the session's guard
  removed, the test fails.
- **Every timed order joined.** Each order in every published run is joined from four records,
  the sender's, the program's, the gateway's journal and the venue's, none missing, its stages in
  order.
- **CI** on every push: x64, ThreadSanitizer, AddressSanitizer/UBSan and arm64. The whole-day
  checks need the sample day and run here, not in CI.

## Findings

- **The measurement found a fault in the gateway.** Every point's maximum was tens of
  milliseconds, at every pace. One engine pass held every order for that long: the gateway's store
  of sent messages, reserved for less than a day, reallocated and copied itself when full. Its
  reserve is now configured, the program sizes it for the day, and the runs were made again
  (note 005; the gateway's note 004).
- **The default is not the fastest.** B, the same work on the feed's thread, is faster at every
  point measured, by the cost of A's ring. A stays the default because its feed never waits on
  the strategy's work, which this strategy, a few hundred nanoseconds a step, does not show
  (note 005).
- **Answers decide the next order.** Two runs of the program agree only on their first tens of
  orders: when the venue's answer arrives relative to the book events decides what is sent next,
  so runs are compared as distributions, not order by order (note 004).
- **Open.** The gateway-and-venue stage grows with the pace on both paths while the gateway
  alone does not; journal A's maximum varies by an order of magnitude between runs. Both are
  recorded in [the open items](docs/open.md), with what would reopen them.

## How it is measured

- **Open loop.** The feed handler's sender paces packets on a fixed schedule and writes each
  message's intended send time to shared memory; the program reads it when it sends the order
  the message caused, and the venue stamps the order's arrival on the same clock, so a stall
  delays every order due during it. Orders caused by an answer, not by a book event, have no tick
  and are counted apart. The sender's own lateness is recorded beside every point.
- **The harness** is lockfree-ipc's, fetched at a pinned commit: TSC clock, percentiles after a
  discarded warm-up, the noise floor and the machine's conditions in every run, and a refusal to
  publish unless the machine is in bench mode and the build is clean.
- **The machine.** HPE ProLiant DL380 Gen9, 2 × Xeon E5-2699 v4, Ubuntu 26.04, kernel 7.0, set
  up as lockfree-ipc describes ([its machine scripts](https://github.com/Mukachi/lockfree-ipc/tree/main/tools/machine)):
  reserved, isolated CPUs on node 0, with services, kernel threads and interrupts fenced off them.
  Over the card, the sender and the venue run in a network namespace on the far port (the feed
  handler's `tools/card-link`), with kernel busy polling.

## The sample day

Nasdaq publishes sample days of TotalView-ITCH 5.0 at emi.nasdaq.com without stated terms, so
none is in this repository and none is redistributed. The feed handler's script, fetched with it
by the first `cmake` (below), downloads the one used here (5.6 GB, size and SHA-256 checked) into
its own `data/`; move it into this repository's `data/`, which git ignores:

```sh
python build/_deps/itch_feed_handler-src/tools/fetch_day.py
mkdir -p data && mv build/_deps/itch_feed_handler-src/data/01302020.NASDAQ_ITCH50.gz data/
```

## Build and test

Linux; C++20; tested with GCC 15.2, CMake 4.2 and Python 3.14.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build -LE data
```

CMake fetches lockfree-ipc, itch-feed-handler and fix-order-gateway at pinned commits into
`build/`, and generates the gateway's FIX 4.4 dictionary from the FIX Trading Community's
orchestrations at the commit and hash the gateway pins. Checks that need the sample day carry the
CTest label `data` (several take an hour or more).

## Running the benchmarks

```sh
build/sink_bench out.json           # the strategy's sink in the feed handler (note 002)
build/risk_bench out.json           # the risk checks (note 003)
build/feed_sender 127.0.0.1 239.192.0.1 26400 26401 5 0 data/01302020.NASDAQ_ITCH50.gz /ttt-stamps &
build/t2t_bench loopback out.json   # tick to trade (note 005)
```

`t2t_bench` starts the program (and, on loopback, the venue) for each point, drives the sender
over its control port and joins what each process recorded. For the card, the sender and
`build/order_venue 10.99.0.2:26500 6 4 build/t2t-venue stamps` run in the namespace on 10.99.0.2
and the benchmark is given `card`. Each benchmark writes one JSON file: the environment, the noise
floor, the clock, then the results. Run without a path, a benchmark writes into `results/`, and
refuses unless the machine is in bench mode and the build is clean; given a path elsewhere, it
runs anywhere and marks the file unpublished. The CPU numbers are this machine's. To publish
runs: list them in `results/published.toml`, then `python run.py report`.

## Design notes

Every component has a design note written and agreed before its code: proposal, design, spec,
tasks, and a review log with every finding, including the wrong turns. Index:
[docs/README.md](docs/README.md).

## Licence

[MIT](LICENSE). Nasdaq, TotalView and ITCH are Nasdaq's; FIX is the FIX Trading Community's; this
is an independent implementation of their published specifications.
