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
The strategy's sink: not yet measured (design note 002).
<!-- /results -->
