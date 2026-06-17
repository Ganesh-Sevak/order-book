# Demo Script

## Build And Test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Point out that tests include isolated doctest cases, deterministic matching scenarios, random invariant/property checks, parser corpus coverage, and an optional LibFuzzer target when the compiler runtime supports it.

## Replay A Fixture

```bash
./build/orderbook_demo replay --input examples/events.jsonl \
  --snapshots /tmp/orderbook_snapshots.jsonl \
  --trades /tmp/orderbook_trades.jsonl
```

Open `web/index.html` and load both generated files. Show the ladder, recent trades, spread, imbalance, and metrics.

For a streamed local demo:

```bash
python3 tools/serve_replay.py --snapshots /tmp/orderbook_snapshots.jsonl --trades /tmp/orderbook_trades.jsonl
```

Open `http://127.0.0.1:8000` and press Connect.

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

The important reviewer signal is not just speed. It is that the output includes raw and corrected p50/p99/p99.9, batch ns/op, throughput, clock-overhead calibration, compiler, seed, and workload.
