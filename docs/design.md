# Design Notes

## Matching Model

The engine implements price-time priority. Better prices execute first; orders at the same price execute FIFO. Limit orders can rest, market orders never rest, IOC cancels any unfilled remainder, and FOK first verifies available opposite-side liquidity before executing.

Prices are integer ticks. Floating-point prices were rejected because binary rounding creates edge cases at comparison boundaries and distracts from matching semantics.

## Data Structures

The book stores each side in a sorted `std::vector<PriceLevel>`. A price level stores aggregate quantity, order count, head index, and tail index. Orders live in a preallocated `std::vector<OrderNode>` and are linked by integer indices rather than pointers. Cancelled and fully filled order slots are returned to a free-list and reused.

Why this shape:

- Sorted vectors keep price-level traversal contiguous and make top-N snapshots cache-friendly.
- Binary search gives O(log M) price lookup while best bid/ask remain O(1) at vector edges.
- Index-based intrusive queues make cancel unlink O(1) once the order ID is found.
- Preallocated order storage plus a free-list avoids hidden hot-path heap growth and avoids exhausting capacity after normal cancels/fills.
- Integer indices keep hot order nodes compact and easier to reason about than owning pointers.

Rejected alternatives:

- A flat price array would make price-level access O(1), but it requires a bounded price band and wastes memory for sparse books.
- `std::map` was initially simple, but every level traversal pointer-chased through tree nodes.
- A pointer-heavy custom tree would be more complex without improving the demo's measured bottlenecks.
- A mutex-protected queue was rejected for ingestion because SPSC has a simple acquire/release protocol and avoids lock convoy behavior.

## Complexity

`N` is live orders and `M` is active price levels.

| Operation | Complexity |
| --- | ---: |
| Add at existing price | O(log M) lookup + O(1) append |
| Add at new price | O(log M) lookup + O(M) insert |
| Cancel | O(1) |
| Replace | O(1) cancel + submit cost |
| Match | O(1) per maker order consumed |
| Best bid/ask | O(1) edge access |
| Depth at exact price | O(log M) |
| Top-N snapshot | O(N levels requested) |

## Concurrency Boundary

`OrderBook` is not thread-safe. The matching core is single-writer by design: exactly one thread owns calls to `submit`, `cancel`, `replace`, and query methods while it is mutating. That preserves deterministic replay and lets invariant tests inspect the full state without racing.

`SpscRing<T>` is the explicit concurrency primitive for moving events or snapshots around the core. The producer writes a slot, then publishes `head` with `memory_order_release`. The consumer reads `head` with `memory_order_acquire`, which guarantees it sees the slot contents before consuming. The consumer releases `tail` after reading so the producer can safely reuse capacity. `head` and `tail` are cache-line padded because each thread writes a different counter.

## Observability

The core exposes allocation-free counters in `Metrics`: submitted, accepted, rejected, canceled, replaced, trades, and capacity rejections. Parser failures, queue drops, and backpressure are `PipelineMetrics` concerns because they belong to replay/session orchestration rather than the book.

The current implementation avoids hot-path logging. Detailed logs belong at component boundaries such as parser errors, replay failures, and artifact generation.
