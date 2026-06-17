# Performance Notes

Benchmarks are reproducible with:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/orderbook_bench --orders 100000 --seed 42
```

Measured locally on macOS 26.5.1, Darwin arm64, AppleClang 21.0.0, Release build.

| Scenario | Throughput/sec | p50 ns | p99 ns | p99.9 ns |
| --- | ---: | ---: | ---: | ---: |
| add_only | 22,042,210 | 41 | 83 | 833 |
| mixed_add_cancel_replace | 17,159,649 | 42 | 84 | 833 |
| aggressive_crossing | 36,335,558 | 0 | 42 | 42 |
| snapshot_top10 | 10,870,061 | 42 | 84 | 125 |
| spsc_ring_throughput | 9,945,877 | 83 | 208 | 250 |

The benchmark prints JSON lines so results can be archived by CI or pasted into this file after each optimization.

## Optimization Log

| Change | Before | Bottleneck evidence | After |
| --- | --- | --- | --- |
| Initial index-based FIFO queues | N/A | Design chosen before baseline. Avoided pointer ownership and O(n) cancel scans. | Baseline above. |
| Preallocated order storage | N/A | Capacity checks are explicit; no allocator profiling has been run yet. | Baseline above. |
| Cache-line padded SPSC counters | N/A | Prevents producer/consumer counter false sharing by construction. | Baseline SPSC above. |

Future optimization entries should include a profiler artifact or command, such as `perf`, Instruments, or a flamegraph. Do not update README performance claims without recording the before number, bottleneck evidence, change, and after number here.
