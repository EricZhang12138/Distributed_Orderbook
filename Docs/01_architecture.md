# Architecture Overview

This system is a single-symbol limit order book engine designed for low-latency order processing. The design is inspired by the LMAX Disruptor pattern: many producer threads, one consumer thread, with a lock-free ring buffer between them.

## High-level diagram

```
   Gateway 0 ─┐
   Gateway 1 ─┼──> RingBuffer_inbound ──> MatchingEngine ──> Orderbook
   Gateway 2 ─┘                                │
                                               ├──> RingBuffer_outbound[0] ──> Gateway 0
                                               ├──> RingBuffer_outbound[1] ──> Gateway 1
                                               └──> RingBuffer_outbound[2] ──> Gateway 2
```

- Many gateways write to **one** shared inbound buffer.
- One engine reads from the inbound buffer and processes each event through the orderbook.
- The engine writes results back to **per-gateway** outbound buffers, routed by `gateway_id`.

## Components

### `Gateway` (`Core_Infra/gateway.{hpp,cpp}`)
The user-facing (or feed-facing) entry point. One gateway corresponds to one *source* of orders — a connected user, a market-data replay adapter, a strategy thread.

Owns:
- A reference (raw pointer) to the shared inbound ring buffer.
- A reference to *its own* outbound ring buffer.
- A `gateway_id` used to route acks back to it.
- A `GatewayMode` (LIVE or REPLAY) — see `04_gateway.md`.

Public methods:
- `place_order_to_ring_buffer(...)` — push a `NEW` event onto the inbound buffer.
- `cancel_order_to_ring_buffer(order_id, ...)` — push a `CANCEL` event.
- `read_from_ring_buffer(...)` — drain one ack from the outbound buffer if available.

Does **not** touch the orderbook directly. Everything goes through the ring buffer.

### `RingBuffer_inbound` (`Core_Infra/ring_buffer.{hpp,cpp}`)
A fixed-size, power-of-two array of `Orderevent_inbound` slots. Multi-producer / single-consumer. Producers claim slots atomically; consumer polls each slot's `is_ready` flag.

Details in `03_ring_buffers.md`.

### `RingBuffer_outbound`
Same shape as inbound but per-gateway. Single-producer (the engine) / single-consumer (one gateway). No atomic write pointer needed because only the engine writes.

### `MatchingEngine` (`Core_Infra/matching_engine.{hpp,cpp}`)
The single consumer of the inbound buffer. Runs an infinite loop:
1. Poll the next inbound slot.
2. If `is_ready`, dispatch on `event->op`:
   - `NEW` → `orderbook.placeOrder(...)`
   - `CANCEL` → `orderbook.cancel(...)`
3. Write the result into the outbound buffer for `event->gateway_id`.
4. Release the inbound slot and advance.

Owns the `Orderbook` instance. Holds raw pointers to the inbound buffer and the vector of outbound buffers.

### `Orderbook` (`Core_Infra/orderbook.{hpp,cpp}`)
The actual matching logic. Two red-black trees (one for bids, one for asks), each price level is a doubly-linked list of orders. Time priority is preserved within each price level (FIFO).

Public API: `placeOrder(...)`, `cancel(order_id)`. Internal: `match(...)`.

Details in `02_orderbook.md`.

## Threading model

- **Gateway threads** (N): each call into `place_order_to_ring_buffer` runs on the gateway's thread. The gateway methods are safe to call from multiple threads if you share one gateway, but typically each gateway is single-threaded.
- **Matching engine thread** (1): runs `MatchingEngine::run()` in a tight loop. Single-threaded by design — matching requires deterministic ordering, so introducing concurrency inside the orderbook would require locks and break price-time priority.
- **No shared mutable state** between gateways and the engine *except* the ring buffer slots, and those are coordinated by the atomic `write_pointer` + the per-slot `is_ready` flag.

## Why this shape

A lock-based design would serialize gateways on a mutex around the orderbook, with all the cost of context switches and contention. A Disruptor-style ring buffer lets each producer make progress independently (each `claim()` is a single `fetch_add`), and the consumer never blocks — it just polls the next slot.

The trade-off: the engine spins at 100% CPU. That is intentional for low-latency systems and you pin it to a core. For dev work you can add a `yield()` after a missed poll.

## Order flow (single order, end to end)

1. User calls `gateway.place_order_to_ring_buffer(price, vol, side, ...)`.
2. Gateway calls `inbound.claim()` → atomic `fetch_add` returns a unique slot index.
3. Gateway fills the slot with the order data.
4. Gateway calls `inbound.publish(slot)` → sets `is_ready = true` with release semantics.
5. Engine, in its loop, reads `is_ready` with acquire semantics. Sees `true`.
6. Engine reads the order data (safe because of acquire/release ordering).
7. Engine calls `orderbook.placeOrder(...)` → returns an `orderResult`.
8. Engine writes that result into `outbound[gateway_id]` at its own write pointer.
9. Engine publishes the outbound slot, releases the inbound slot, advances both pointers.
10. Gateway, on its own time, calls `read_from_ring_buffer(...)` and picks up the ack.

## What the system does **not** do today

- Multi-symbol support (one `Orderbook` instance only).
- Ring-buffer backpressure (a fast producer can wrap the buffer and overwrite unread slots).
- `MODIFY` op-code (only `NEW` and `CANCEL`).
- Persistence / recovery.
- Network I/O — gateways are in-process.

These are tracked as follow-ups; see `REPLAY_MODE_PLAN.md` and the wider review.
