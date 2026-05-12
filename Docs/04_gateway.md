# Gateway

The `Gateway` is the boundary between the outside world and the matching engine. Anything that produces orders — a connected user, a market-data replay adapter, an algorithmic strategy — gets its own `Gateway` instance.

## What the gateway does (and what it does not)

It does:
- Translate the caller's intent (place / cancel) into an `Orderevent_inbound` and push it onto the shared inbound ring buffer.
- Assign an internal order id (in LIVE mode).
- Stamp the event with a timestamp (in LIVE mode).
- Drain its own outbound ring buffer when the caller asks for acks.

It does **not**:
- Touch the orderbook directly.
- Talk to other gateways.
- Know anything about matching, price levels, or fills — those are the engine's responsibility.

A gateway is intentionally thin. All correctness lives in the orderbook + engine; the gateway is a typed adapter.

## Construction

```cpp
Gateway(RingBuffer_inbound* buffer_inbound,
        RingBuffer_outbound* ring_buffer_outbound,
        int64_t id,
        GatewayMode mode = GatewayMode::LIVE);
```

Parameters:
- `buffer_inbound` — raw pointer to the shared inbound ring buffer. The gateway does not own it; the gateway just writes through it.
- `ring_buffer_outbound` — raw pointer to *this* gateway's outbound ring buffer. One per gateway. The gateway is its only reader.
- `id` — the `gateway_id`. The engine uses this field on the inbound event to know which outbound buffer to write the ack into. Must match the index used by the engine's `ring_buffer_outbound[gid]` vector.
- `mode` — see below.

Raw pointers (rather than `unique_ptr`) are deliberate: the ring buffers are shared, and ownership lives elsewhere (typically the `main` function or whoever sets up the system). The gateway is a borrower.

## Gateway modes

The gateway supports two modes, set at construction and immutable afterward:

```cpp
enum class GatewayMode { LIVE, REPLAY };
```

### LIVE mode

The default. Used for real user-facing or strategy-facing traffic.

- `place_order_to_ring_buffer(price, vol, side, username)` mints a fresh internal order id via `internal_id_counter.fetch_add(1)`.
- `order_arrival_time` is set to `getCurrentTime()` (wall clock in nanoseconds since epoch).
- External args, if passed, are ignored.

### REPLAY mode

Used by data-replay adapters: anything that's feeding the orderbook with events whose ids and timestamps already exist (LOBSTER CSVs, NASDAQ ITCH binary feeds, crypto websocket history, etc.).

- `place_order_to_ring_buffer(price, vol, side, username, external_id, external_ts)` uses `external_id` and `external_ts` directly. Both must be `>= 0`; asserts otherwise.
- `cancel_order_to_ring_buffer(order_id, external_ts)` uses `external_ts`. Asserts otherwise.

Returns the id that was actually used (which is just `external_id` in REPLAY mode), so the caller can store it and cancel by that id later, exactly like in LIVE mode.

### Why mode is a property of the gateway, not a per-call switch

A replay adapter and a live user-facing endpoint are different kinds of producers. Mixing the two on one gateway instance would mean:
- Half the calls auto-generate ids; the other half don't. Easy to get wrong.
- The `gateway_id` is a routing tag for acks — it doesn't make sense to share it between two unrelated data sources.

Construct two gateways instead. They share the inbound buffer at no cost.

## Methods

### `place_order_to_ring_buffer`

```cpp
int64_t place_order_to_ring_buffer(int64_t price, int64_t volume, bool side,
                                    std::string username,
                                    int64_t external_id = -1,
                                    int64_t external_ts = -1);
```

Returns the order id used (so the caller can later cancel by that id).

Flow:
1. Decide `order_id` and `arrival_ts` based on mode.
2. `claim()` a slot from the inbound buffer.
3. Write `op = NEW`, the order fields, and `gateway_id` into the slot.
4. `publish()` the slot — release-store on `is_ready`.

The default arguments make the LIVE call site identical to the old (pre-replay) API. No existing call sites had to change.

### `cancel_order_to_ring_buffer`

```cpp
void cancel_order_to_ring_buffer(int64_t order_id, int64_t external_ts = -1);
```

Same shape as `place`, but with `op = CANCEL` and only `internal_order_id` filled in. `price` / `volume` / `side` are unused for cancels and left at their default values.

### `read_from_ring_buffer`

```cpp
bool read_from_ring_buffer(Fill* fills, int64_t& fill_count,
                           int64_t& order_id, bool& fulfilled,
                           int64_t& remaining_qty);
```

Tries to drain one ack from the outbound buffer.

- Returns `true` if an ack was available and copied into the caller's buffers.
- Returns `false` if the current slot's `is_ready` is still `false` (engine hasn't published yet).

The caller is responsible for polling. The gateway keeps its own outbound read pointer (`outbound_ringbuffer_read_p`) — incremented only on successful reads.

Ack format conventions today:
- For a NEW: `fulfilled` = whether the order was fully filled; `remaining_qty` = leftover that rested; `fill_count` + `fills[]` = the trades produced.
- For a CANCEL: `fulfilled` = whether the cancel succeeded; `fill_count` = 0; `remaining_qty` = 0.

The CANCEL convention reuses the `fulfilled` field. This is a minor abuse — a follow-up plan is to add an explicit `op` field to `Orderevent_outbound`.

## Per-gateway outbound buffer — why

Each gateway has its own outbound buffer rather than sharing one. This is for two reasons:

1. **Routing simplicity.** The engine knows the destination from `event->gateway_id` and indexes straight into `outbound[gid]`. No filtering by id needed on the read side.
2. **No contention on the consumer side.** Each gateway reads only its own buffer. If gateways shared an outbound, every gateway would have to scan past acks that aren't theirs, or some routing layer would need to demux.

The cost: memory. With N gateways and buffer size B, you spend N × B × sizeof(event). For B = 1024 and N = 4 that's about 256 KB — negligible.

## Lifecycle and ownership

Typical setup (see `main.cpp` for the live example):

```cpp
RingBuffer_inbound inbound(1024);

std::vector<std::unique_ptr<RingBuffer_outbound>> outbounds;
for (int i = 0; i < N; i++)
    outbounds.push_back(std::make_unique<RingBuffer_outbound>(1024));

std::vector<RingBuffer_outbound*> outbound_ptrs;
for (auto& ob : outbounds) outbound_ptrs.push_back(ob.get());

MatchingEngine engine(&inbound, outbound_ptrs);

std::vector<std::unique_ptr<Gateway>> gateways;
for (int i = 0; i < N; i++)
    gateways.push_back(std::make_unique<Gateway>(&inbound, outbounds[i].get(), i, GatewayMode::LIVE));
```

Both gateway and outbound vectors hold `unique_ptr` rather than the objects directly. The reason: both `Gateway` and `RingBuffer_outbound` contain `std::atomic` members, which are not copyable or movable. A `std::vector<Gateway>` cannot resize (it'd have to move elements during reallocation), but a `std::vector<std::unique_ptr<Gateway>>` can — it only moves pointers.

## Adding a new mode

The current shape — `enum class GatewayMode` + a branch inside the two writer methods — was chosen because there are only two modes today. If a third mode appears (for example, a "shadow" mode that mirrors traffic to two engines), the right refactor is to extract a `IdAndTimestampStrategy` policy struct rather than keep growing the if/else. Until then the branch is fine.

## Things this doc deliberately doesn't cover

- The exact wire format of LOBSTER / ITCH messages — that belongs in a future "Replay Adapter" doc once the adapter is built.
- Engine-side dispatch on `op` — see `01_architecture.md` and `matching_engine.cpp`.
- Memory ordering and the `claim` / `publish` / `release` protocol — see `03_ring_buffers.md`.
