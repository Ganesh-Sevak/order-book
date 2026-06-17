# Design Notes

## Matching Model

The engine implements price-time priority. Better prices execute first; orders at the same price execute FIFO. Limit orders can rest, market orders never rest, IOC cancels any unfilled remainder, and FOK first verifies available opposite-side liquidity before executing.

Prices are integer ticks. Floating-point prices were rejected because binary rounding creates edge cases at comparison boundaries and distracts from matching semantics.

## Data Structures

The book stores each side in `std::map<Price, PriceLevel>`. A price level stores aggregate quantity, order count, head index, and tail index. Orders live in a preallocated `std::vector<OrderNode>` and are linked by integer indices rather than pointers.

Why this shape:

- `std::map` keeps price levels ordered with O(log M) insertion for new levels and simple edge access for best bid/ask.
- Index-based intrusive queues make cancel unlink O(1) once the order ID is found.
- Preallocated order storage avoids hidden hot-path heap growth until configured capacity is reached.
- Integer indices keep hot order nodes compact and easier to reason about than owning pointers.

Rejected alternatives:

- A flat price array would make price-level access O(1), but it requires a bounded price band and wastes memory for sparse books.
- A pointer-heavy tree of custom nodes would be more complex without improving the demo's measured bottlenecks.
- A mutex-protected queue was rejected for ingestion because SPSC has a simple acquire/release protocol and avoids lock convoy behavior.

## Complexity

`N` is live orders and `M` is active price levels.

| Operation | Complexity |
| --- | ---: |
| Add at existing price | O(1) after tree lookup |
| Add at new price | O(log M) |
| Cancel | O(1) |
| Replace | O(1) cancel + submit cost |
| Match | O(1) per maker order consumed |
| Best bid/ask | O(1) edge access |
| Depth at exact price | O(log M) |
| Top-N snapshot | O(N levels requested) |

## Concurrency Boundary

The matching core is single-writer by design. That preserves deterministic replay and lets invariant tests inspect the full state without racing.

`SpscRing<T>` is the explicit concurrency primitive for moving events or snapshots around the core. The producer writes a slot, then publishes `head` with `memory_order_release`. The consumer reads `head` with `memory_order_acquire`, which guarantees it sees the slot contents before consuming. The consumer releases `tail` after reading so the producer can safely reuse capacity. `head` and `tail` are cache-line padded because each thread writes a different counter.

## Observability

The core exposes allocation-free counters in `Metrics`: submitted, accepted, rejected, canceled, replaced, trades, parser failures, queue drops, and capacity rejections. Demo tools emit structured JSON lines for errors and summary output so failures are scriptable.

The current implementation avoids hot-path logging. Detailed logs belong at component boundaries such as parser errors, replay failures, and artifact generation.
