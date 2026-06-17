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

For a live-feeling local stream:

```bash
python3 tools/serve_replay.py --snapshots /tmp/orderbook_snapshots.jsonl --trades /tmp/orderbook_trades.jsonl
```

Then open `http://127.0.0.1:8000` and connect to the default SSE URL.

## What It Implements

- Price-time priority matching with integer tick prices.
- Limit, market, IOC, FOK, cancel, and cancel-replace flows.
- FIFO queues per price level.
- O(1) order lookup for cancels/replaces via `OrderId -> order index`.
- O(1) best bid/ask access at sorted-vector edges.
- Preallocated order storage with free-list slot reuse and explicit capacity rejection.
- Small-buffer trade results for common 0-4 fill paths.
- Lock-free SPSC ring buffer with acquire/release memory-ordering comments.
- Defensive JSONL parser, replay CLI, synthetic generator, doctest tests, parser corpus, and optional LibFuzzer target.

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
index-based FIFO level queues         static/SSE web visualizer
```

The matching core is intentionally single-writer. Concurrency belongs around the core, not inside it, so replay is deterministic and invariant checks are straightforward. `SpscRing<T>` is provided for ingestion/publishing pipelines where one producer hands events to one consumer without a mutex.

## Complexity

| Operation | Complexity | Note |
| --- | ---: | --- |
| Submit at existing resting level | O(log M) lookup + O(1) append | Binary-searches sorted levels, then appends to FIFO. |
| Submit at new price level | O(log M) lookup + O(M) insert | `M` is active price levels; vector insertion shifts levels. |
| Match best level | O(1) per maker fill | Removes from FIFO head. |
| Cancel live order | O(1) | Hash lookup plus intrusive unlink. |
| Replace live order | O(1) cancel + submit cost | Exchange-style cancel-replace loses time priority. |
| Best bid / ask | O(1) | Sorted-vector edge access. |
| Snapshot top N | O(N) | Walks best levels only. |

## Local Benchmark Snapshot

Command:

```bash
./build/orderbook_bench --orders 100000 --seed 42
```

Environment: macOS 26.5.1, Darwin arm64, AppleClang 21.0.0, Release build.

Clock overhead calibration reported `0 ns` on this run, so raw and corrected percentiles are identical.

| Scenario | Throughput/sec | Batch ns/op | Corrected p50 ns | Corrected p99 ns | Corrected p99.9 ns |
| --- | ---: | ---: | ---: | ---: | ---: |
| add_only | 18,034,401 | 55 | 42 | 84 | 875 |
| mixed_add_cancel_replace | 14,426,023 | 69 | 42 | 125 | 167 |
| aggressive_crossing | 31,124,374 | 32 | 0 | 42 | 42 |
| snapshot_top10 | 16,826,773 | 59 | 41 | 42 | 125 |
| spsc_ring_throughput | 10,058,295 | 99 | 42 | 250 | 292 |

The sub-100 ns figures are close to local clock resolution; treat them as comparative local measurements, not portable latency guarantees. Every performance claim should be regenerated on the target machine.

## Repository Map

- `include/orderbook/` - public API, types, parser contract, SPSC ring.
- `src/` - matching engine, JSONL parser, synthetic workload generator.
- `apps/orderbook_demo.cpp` - replay and synthetic artifact generation.
- `bench/orderbook_bench.cpp` - reproducible percentile benchmarks.
- `tests/` - doctest unit/property tests, parser corpus, and optional LibFuzzer entry point.
- `docs/` - design, performance log, and demo script.
- `web/` - static artifact visualizer.
- `tools/` - local replay/SSE helper scripts.

## Dependency Policy

The C++ core uses only the standard library and CMake. `doctest` is vendored as a single-header test dependency because isolated tests and assertion diagnostics are worth the small surface area. The visualizer is plain HTML/CSS/JS, and the optional replay server uses only the Python standard library.
