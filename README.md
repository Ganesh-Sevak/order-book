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
- Fixed-capacity, open-addressed `OrderId -> order index` lookup with no hot-path rehashes or node allocations.
- Configured dense tick ladder with hierarchical occupancy bitmaps for bounded best-price discovery.
- Preallocated order storage with free-list slot reuse and explicit capacity rejection.
- Small-buffer trade results for common 0-4 fill paths.
- Optional synchronous `TradeSink` API that emits trades without constructing a result-owned trade list.
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

## Configuration and Complexity

Every book requires an explicit inclusive price band. The band is allocated at construction and never grows:

```cpp
OrderBook book({.min_price = 9'000, .max_price = 11'000, .max_orders = 1'000'000});
```

Limit prices outside that band are rejected as invalid; market-order prices are ignored. `submit(order, TradeSink)` and `replace(order, TradeSink)` return compact summaries and invoke a non-null `noexcept` callback for every trade. The callback must not re-enter the book.

| Operation | Complexity | Note |
| --- | ---: | --- |
| Submit at any configured resting level | O(1) level access + O(1) append | No level insertion, erase, or binary search. |
| Match best level | O(1) per maker fill | Removes from FIFO head. |
| Cancel live order | Expected O(1) | Flat ID-index lookup plus intrusive unlink. |
| Replace live order | Expected O(1) + submit cost | Exchange-style cancel-replace loses time priority. |
| Best bid / ask | O(log64 P) | Bitmap hierarchy over `P` configured ticks. |
| Snapshot top N | O(N log64 P) | Visits occupied levels only. |

## Local Benchmark Snapshot

Command:

```bash
./build/orderbook_bench --orders 100000 --seed 42 --runs 5 > candidate.jsonl
```

Environment: macOS 26.5.1, Darwin arm64, AppleClang 21.0.0, Release build.

The benchmark performs one warmup suite and reports median values across the requested runs. It includes `deep_wide_level_churn`, which repeatedly creates and empties dispersed levels in a 131,072-tick band. Compare same-machine, same-command artifacts with `python3 tools/check_bench.py baseline.jsonl candidate.jsonl`; the gate uses raw p99 because clock-overhead calibration can vary by a timer tick between runs.

The sub-100 ns figures are close to local clock resolution; treat them as comparative local measurements, not portable latency guarantees.

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
