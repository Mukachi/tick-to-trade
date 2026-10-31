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
Pre-trade risk: not yet measured (design note 003).
<!-- /results -->
