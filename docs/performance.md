# Performance Notes

Build the Release benchmark and write a comparable JSONL artifact:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/orderbook_bench --orders 100000 --seed 42 --runs 5 > candidate.jsonl
```

The benchmark runs one warmup suite, then reports median p50/p99/p99.9, batch latency, and throughput across the requested number of runs. It includes these matching-core scenarios:

| Scenario | Purpose |
| --- | --- |
| `add_only` | Resting orders at a small set of hot levels. |
| `mixed_add_cancel_replace` | Typical add/cancel/replace churn. |
| `aggressive_crossing` | Short crossing/IOC execution paths. |
| `deep_wide_level_churn` | Repeatedly creates and empties dispersed levels in a 131,072-tick band. |
| `snapshot_top10` | Read-side top-of-book extraction. |

The deep/wide scenario is the primary tail-latency check for price-level maintenance. The SPSC result remains an ingestion reference rather than a matching-core gate.

## Comparison Gate

Capture the baseline from the pre-change revision on the same machine and command, then compare:

```bash
python3 tools/check_bench.py baseline.jsonl candidate.jsonl
```

The gate requires at least 25% lower raw p99 for `deep_wide_level_churn` when both artifacts contain it and permits no more than a 5% raw-p99 regression in shared existing scenarios. Corrected percentiles remain diagnostic output because clock-overhead calibration can vary by a timer tick between runs. The script rejects artifacts that differ in compiler, C++ mode, event count, or seed.

Sub-100 ns figures are close to local clock resolution. Use them for same-host comparisons, not portable latency guarantees. Pair each performance result with a profiler artifact or command such as `sample`, Instruments, or a flamegraph.

## Optimization Log

| Change | Before | Bottleneck evidence | After |
| --- | --- | --- | --- |
| Initial index-based FIFO queues | N/A | Avoided pointer ownership and O(n) cancel scans. | Baseline architecture. |
| Free-list order slot reuse | Dead order slots were not reused. | Review found capacity exhausted after normal cancels/fills. | Cancelled/filled slots are reusable. |
| Dense price ladder + occupancy bitmap | Sorted-vector price levels. | New-level inserts and empty-level erases shifted O(M) entries. | Requires same-host baseline/candidate artifact comparison. |
| Fixed flat order index | Node-based `std::unordered_map`. | Rehashes and per-entry allocations create avoidable tail work. | Preallocated open-addressed table with backward-shift deletion. |
| TradeSink fast path | Result-owned `TradeList` for all callers. | Passive orders constructed result storage even when callers consume executions synchronously. | Opt-in compact summary plus synchronous callback. |
