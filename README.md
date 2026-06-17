# Order Book

A C++23 limit order book and matching-engine project built to show systems judgment: deterministic matching, explicit data-structure tradeoffs, lock-free ingestion primitives, reproducible benchmarks, parser fuzzing, and a small artifact visualizer.

## Quick Demo

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

./build/orderbook_demo synthetic --orders 100000 --seed 42 \
  --snapshots /tmp/orderbook_snapshots.jsonl \
  --trades /tmp/orderbook_trades.jsonl

./build/orderbook_bench --orders 100000 --seed 42
```

Open `web/index.html` in a browser and load `/tmp/orderbook_snapshots.jsonl` plus `/tmp/orderbook_trades.jsonl` to inspect the generated book state.

## What It Implements

- Price-time priority matching with integer tick prices.
- Limit, market, IOC, FOK, cancel, and cancel-replace flows.
- FIFO queues per price level.
- O(1) order lookup for cancels/replaces via `OrderId -> order index`.
- O(1) best bid/ask access using map edge iterators.
- Preallocated order storage with explicit capacity rejection.
- Lock-free SPSC ring buffer with acquire/release memory-ordering comments.
- Defensive JSONL parser, replay CLI, synthetic generator, tests, and fuzz smoke target.

## Architecture

```text
JSONL / synthetic events
        |
        v
orderbook_demo / orderbook_bench
        |
        v
single-writer OrderBook core  ---> trades / snapshots / metrics JSONL
        |                                  |
        v                                  v
index-based FIFO level queues         static web visualizer
```

The matching core is intentionally single-writer. Concurrency belongs around the core, not inside it, so replay is deterministic and invariant checks are straightforward. `SpscRing<T>` is provided for ingestion/publishing pipelines where one producer hands events to one consumer without a mutex.

## Complexity

| Operation | Complexity | Note |
| --- | ---: | --- |
| Submit at existing resting level | O(1) after lookup | Appends to level FIFO queue. |
| Submit at new price level | O(log M) | `M` is active price levels. |
| Match best level | O(1) per maker fill | Removes from FIFO head. |
| Cancel live order | O(1) | Hash lookup plus intrusive unlink. |
| Replace live order | O(1) cancel + submit cost | Exchange-style cancel-replace loses time priority. |
| Best bid / ask | O(1) | Map edge iterator access. |
| Snapshot top N | O(N) | Walks best levels only. |

## Local Benchmark Snapshot

Command:

```bash
./build/orderbook_bench --orders 100000 --seed 42
```

Environment: macOS 26.5.1, Darwin arm64, AppleClang 21.0.0, Release build.

| Scenario | Throughput/sec | p50 ns | p99 ns | p99.9 ns |
| --- | ---: | ---: | ---: | ---: |
| add_only | 22,042,210 | 41 | 83 | 833 |
| mixed_add_cancel_replace | 17,159,649 | 42 | 84 | 833 |
| aggressive_crossing | 36,335,558 | 0 | 42 | 42 |
| snapshot_top10 | 10,870,061 | 42 | 84 | 125 |
| spsc_ring_throughput | 9,945,877 | 83 | 208 | 250 |

The sub-100 ns figures are close to local clock resolution; treat them as comparative local measurements, not portable latency guarantees. Every performance claim should be regenerated on the target machine.

## Repository Map

- `include/orderbook/` - public API, types, parser contract, SPSC ring.
- `src/` - matching engine, JSONL parser, synthetic workload generator.
- `apps/orderbook_demo.cpp` - replay and synthetic artifact generation.
- `bench/orderbook_bench.cpp` - reproducible percentile benchmarks.
- `tests/` - deterministic unit/property tests and parser fuzz smoke target.
- `docs/` - design, performance log, and demo script.
- `web/` - static artifact visualizer.

## Dependency Policy

The C++ core uses only the standard library and CMake. This keeps the project easy to build in interviews and prevents dependencies from hiding the data-structure and concurrency decisions. The visualizer is plain HTML/CSS/JS for the same reason.
