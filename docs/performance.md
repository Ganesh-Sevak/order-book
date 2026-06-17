# Performance Notes

Benchmarks are reproducible with:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/orderbook_bench --orders 100000 --seed 42
```

Measured locally on macOS 26.5.1, Darwin arm64, AppleClang 21.0.0, Release build.

Clock overhead calibration reported `0 ns` on this run, so raw and corrected percentiles are identical.

| Scenario | Throughput/sec | Batch ns/op | Corrected p50 ns | Corrected p99 ns | Corrected p99.9 ns |
| --- | ---: | ---: | ---: | ---: | ---: |
| add_only | 18,034,401 | 55 | 42 | 84 | 875 |
| mixed_add_cancel_replace | 14,426,023 | 69 | 42 | 125 | 167 |
| aggressive_crossing | 31,124,374 | 32 | 0 | 42 | 42 |
| snapshot_top10 | 16,826,773 | 59 | 41 | 42 | 125 |
| spsc_ring_throughput | 10,058,295 | 99 | 42 | 250 | 292 |

The benchmark prints JSON lines so results can be archived by CI or pasted into this file after each optimization.

## Optimization Log

| Change | Before | Bottleneck evidence | After |
| --- | --- | --- | --- |
| Initial index-based FIFO queues | N/A | Design chosen before baseline. Avoided pointer ownership and O(n) cancel scans. | Baseline above. |
| Free-list order slot reuse | Rejected new orders after slot vector reached capacity, even when cancelled slots existed. | Review found dead `OrderNode` slots were never reused. | Cancelled/filled slots are reusable; regression tests cover both paths. |
| Sorted-vector price levels | `std::map` price levels. | Review identified tree pointer-chasing for clustered active prices. | Contiguous level traversal and O(1) edge best bid/ask; numbers above are new baseline. |
| Clock-overhead calibration | Percentiles included measurement overhead without reporting it. | Review identified two `Clock::now()` calls around every operation. | Output includes clock overhead, raw percentiles, corrected percentiles, and batch ns/op. |
| Cache-line padded SPSC counters | N/A | Prevents producer/consumer counter false sharing by construction. | Baseline SPSC above. |

Future optimization entries should include a profiler artifact or command, such as `perf`, Instruments, or a flamegraph. Do not update README performance claims without recording the before number, bottleneck evidence, change, and after number here.
