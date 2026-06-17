# Demo Script

## Build And Test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Point out that tests include deterministic matching cases, random invariant/property checks, and parser fuzz smoke coverage.

## Replay A Fixture

```bash
./build/orderbook_demo replay --input examples/events.jsonl \
  --snapshots /tmp/orderbook_snapshots.jsonl \
  --trades /tmp/orderbook_trades.jsonl
```

Open `web/index.html` and load both generated files. Show the ladder, recent trades, spread, imbalance, and metrics.

## Synthetic Load

```bash
./build/orderbook_demo synthetic --orders 100000 --seed 42 \
  --snapshots /tmp/orderbook_snapshots.jsonl \
  --trades /tmp/orderbook_trades.jsonl
```

This is the fastest way to generate larger visualizer artifacts.

## Benchmark

```bash
./build/orderbook_bench --orders 100000 --seed 42
```

The important reviewer signal is not just speed. It is that the output includes p50, p99, p99.9, throughput, compiler, seed, and workload.
