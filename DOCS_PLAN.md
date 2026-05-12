# Docs Plan

Four documentation files under `Docs/`. Each covers one slice of the system, written so a reader who knows C++ but is new to LOBs can follow.

## Files

### `Docs/01_architecture.md` — overall structure
- High-level diagram of components and message flow
- One section per class: `Gateway`, `RingBuffer_inbound`, `RingBuffer_outbound`, `MatchingEngine`, `Orderbook` — what each owns, what it does, what it does not do
- Threading model: many gateway threads + one engine thread
- Why this shape (LMAX Disruptor inspiration)

### `Docs/02_orderbook.md` — LOB internals
- Two red-black trees (`bids`, `asks`), why std::map is the right choice for now
- `Limit` struct = head of doubly linked list at one price level
- `Order` struct = node, with `prev` / `next` / `parentlimit`
- `global_map`: order_id → Order* — purpose and what's in it
- Key performance points: intrusive list, O(1) cancel via `parentlimit`, why std::list was rejected
- `match()` walk-through (one side, since the other is mirrored)
- `cancel()` walk-through

### `Docs/03_ring_buffers.md` — inbound + outbound
- Roles: inbound = MPSC (many gateways, one engine); outbound = SPSC (one engine, one gateway per buffer)
- The slot ownership protocol: claim → get → fill → publish → engine reads → release
- Why `claim()` uses `fetch_add` (atomic ticket) — multiple gateways never collide on a slot
- Memory ordering: release on publish, acquire on read, why
- `is_ready` per slot: the handoff signal
- Power-of-2 size + bitmask indexing
- Cache-line alignment on `Orderevent_inbound` / `Orderevent_outbound`
- Why outbound write_pointer doesn't need to be atomic
- Known gaps: backpressure check not yet implemented

### `Docs/04_gateway.md` — gateway
- Role: translates user/feed actions into ring-buffer events; reads acks back
- Constructor parameters: inbound RB, outbound RB, gateway_id, mode
- LIVE mode: mints internal id, stamps wall-clock
- REPLAY mode: caller supplies external id + timestamp (LOBSTER, ITCH, websocket feeds)
- `place_order_to_ring_buffer`, `cancel_order_to_ring_buffer`, `read_from_ring_buffer`
- Per-gateway outbound buffer rationale
- How to add a new mode in future

## Out of scope (deferred)
- A "how to ingest LOBSTER data" tutorial doc — separate doc once the replay adapter exists
- Performance / benchmark numbers — needs measurement first
