# Design Notes

## Matching Model

The engine implements price-time priority. Better prices execute first; orders at the same price execute FIFO. Limit orders can rest, market orders never rest, IOC cancels any unfilled remainder, and FOK first verifies available opposite-side liquidity before executing.

Prices are integer ticks. Floating-point prices were rejected because binary rounding creates edge cases at comparison boundaries and distracts from matching semantics.

## Data Structures

Every book is constructed with an inclusive, fixed positive tick band. Each side stores a dense `PriceLevel` ladder indexed by `price - min_price`; a level stores aggregate quantity, order count, head index, and tail index. A hierarchical bitmap records which ladder entries are occupied. Orders live in a preallocated `std::vector<OrderNode>` and are linked by integer indices rather than pointers. Cancelled and fully filled order slots are returned to a free-list and reused.

Why this shape:

- Direct tick indexing removes binary searches, vector insertion/erase shifts, and price-level allocation from submit/cancel/match paths.
- The occupancy bitmap finds the best or next occupied level in O(log64 P), without walking empty ticks; `P` is the configured band size.
- Index-based intrusive queues make cancel unlink O(1) once the order ID is found.
- Preallocated order storage, a free-list, and a fixed open-addressed ID index avoid hidden hot-path heap growth and rehashing.
- Compact integer-indexed nodes keep the matching working set small.

Rejected alternatives:

- An adaptive price range was rejected because growth would allocate and create latency spikes; prices outside the configured band are rejected.
- A sparse tree/map was rejected because it reintroduces pointer chasing and does not give predictable level-update latency.
- A mutex-protected queue was rejected for ingestion because SPSC has a simple acquire/release protocol and avoids lock convoy behavior.

## Complexity

`N` is live orders and `P` is configured ticks.

| Operation | Complexity |
| --- | ---: |
| Add at any price in the configured band | Expected O(1) ID check + O(1) append |
| Cancel | O(1) |
| Replace | O(1) cancel + submit cost |
| Match | O(1) per maker order consumed |
| Best bid/ask | O(log64 P) |
| Depth at exact price | O(1) |
| Top-N snapshot | O(N log64 P) |

## Concurrency Boundary

`OrderBook` is not thread-safe. The matching core is single-writer by design: exactly one thread owns calls to `submit`, `cancel`, `replace`, and query methods while it is mutating. That preserves deterministic replay and lets invariant tests inspect the full state without racing.

`SpscRing<T>` is the explicit concurrency primitive for moving events or snapshots around the core. The producer writes a slot, then publishes `head` with `memory_order_release`. The consumer reads `head` with `memory_order_acquire`, which guarantees it sees the slot contents before consuming. The consumer releases `tail` after reading so the producer can safely reuse capacity. `head` and `tail` are cache-line padded because each thread writes a different counter.

## Observability

The core exposes allocation-free counters in `Metrics`: submitted, accepted, rejected, canceled, replaced, trades, and capacity rejections. `submit` and `replace` retain result-owned `TradeList` APIs for convenience. Their `TradeSink` overloads instead return compact summaries and synchronously call a non-null `noexcept` callback for each trade; the callback must not re-enter the book. Parser failures, queue drops, and backpressure are `PipelineMetrics` concerns because they belong to replay/session orchestration rather than the book.

The current implementation avoids hot-path logging. Detailed logs belong at component boundaries such as parser errors, replay failures, and artifact generation.
