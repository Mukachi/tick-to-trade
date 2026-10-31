# tick-to-trade

The path from market data to an order on the wire, built from three repos:
[itch-feed-handler](https://github.com/Mukachi/itch-feed-handler) (Nasdaq TotalView-ITCH 5.0 in),
[lockfree-ipc](https://github.com/Mukachi/lockfree-ipc) (the queues and the measurement harness), pre-trade risk
from EU RTS 6 Article 15 and SEC Rule 15c3-5, and
[fix-order-gateway](https://github.com/Mukachi/fix-order-gateway) (FIX 4.4 out). Work in progress: the plan is
[design note 001](docs/design/001-scope.md); every component is a design note in [docs/](docs/README.md).

## Results

Figures are generated from committed runs by `python run.py report`; every run states its machine and
conditions in [RESULTS.md](RESULTS.md).

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

<!-- results:risk-summary -->
| Case | M events/s | Book event p50 | p99 | p99.9 | Check p50 | p99 | Answer p50 | p99 | Intents | Sent | Refused |
|---|---|---|---|---|---|---|---|---|---|---|---|
| no checks | 12.26–12.27 | 23 | 105–106 | 247–249 | 21 | 29 | 22 | 78–79 | 935,763 | 935,763 | 0 |
| checks | 10.85–10.88 | 34 | 118–119 | 260–262 | 25 | 40–41 | 32 | 128–129 | 1,109,961 | 60,734 | 1,049,227 |
| reference | 1.39–1.45 | 79 | 3,724–4,361 | 14.8 µs–21.8 µs | 1,789–1,829 | 14.4 µs–20.7 µs | 594–649 | 16.3 µs–24.2 µs | 1,109,961 | 60,734 | 1,049,227 |

The strategy's thread on one CPU over the sample day's first 50,000,000 messages' book events for 100 instruments, with no checks, the risk checks, or their reference implementation; ns, range over runs. The method: [RESULTS.md](RESULTS.md).
<!-- /results -->
