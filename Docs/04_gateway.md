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

### Silent flag — a separate axis

Independent of `GatewayMode`, every gateway also has a `silent` flag controlling whether the engine writes outbound acks for events stamped by this gateway.

```cpp
Gateway(RingBuffer_inbound* buffer_inbound,
        RingBuffer_outbound* ring_buffer_outbound,
        int64_t id,
        GatewayMode mode = GatewayMode::LIVE,
        std::optional<bool> silent_override = std::nullopt);
```

Defaults:
- LIVE → `silent = false` (acks emitted, as before).
- REPLAY → `silent = true` (no acks).

Pass `silent_override = true/false` to flip either default explicitly.

#### Why a separate axis instead of a third enum value

`GatewayMode` and `silent` answer two different questions:
- *Where does the id/timestamp come from?* (LIVE vs REPLAY)
- *Does the engine ack?* (silent vs noisy)

All four combinations are valid:

| GatewayMode | silent | scenario |
|---|---|---|
| LIVE  | false (default) | real user/strategy traffic, normal acks |
| LIVE  | true            | shadow traffic / load test / fire-and-forget generator |
| REPLAY| true (default)  | LOBSTER replay — validation reads book state, ack stream wasted |
| REPLAY| false           | replay you want to *observe* (e.g., diff acks against LOBSTER trades) |

Folding silent into the enum (`SILENT_REPLAY`) would conflate the two axes and require a fourth value for `SILENT_LIVE`. Two booleans are cleaner than a 4-state enum.

#### How it's wired

Every `Orderevent_inbound` carries a `bool silent` field, stamped by the gateway in `place/cancel/modify`. The engine reads it once per event:

```cpp
if (event->silent) {
    // run orderbook side-effect only, skip outbound write entirely
    // — and crucially, do NOT advance write_p_outbound[gid]
} else {
    // existing outbound write block
}
```

The silent path still updates the orderbook (matches, cancels, modifies). It just doesn't produce an ack. The outbound write pointer for that gateway never advances, so the outbound buffer is permanently untouched — no risk of overflow, no wasted cache-line stores.

#### Why this matters for REPLAY

LOBSTER message files have hundreds of thousands of events per day per symbol. Without silent mode, the engine would write hundreds of thousands of acks into a buffer nobody reads, overflowing the outbound buffer thousands of times over and clobbering its own writes. Silent mode makes the outbound path zero-cost for replay.

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

### `modify_order_to_ring_buffer`

```cpp
void modify_order_to_ring_buffer(int64_t order_id, int64_t new_size, int64_t external_ts = -1);
```

Reduces an existing order's size in place. The order keeps its queue position (see `02_orderbook.md` for why this matters).

The event format is: `op = MODIFY`, `internal_order_id = order_id`, and the `volume` field is **reused to carry the new size**. `price` and `side` are unused.

**Contract:**
- `new_size` must satisfy `0 < new_size < current_volume`. The orderbook rejects everything else.
- For `new_size == 0`, send a `cancel_order_to_ring_buffer` instead.
- Size-up is not supported (real exchanges decompose it to cancel + new at the back of the queue).
- LIVE mode: stamps `external_ts` with `getCurrentTime()`; REPLAY mode: caller supplies a non-negative `external_ts`.

This is the gateway-side counterpart of LOBSTER message type 2 (partial cancellation).

### `read_from_ring_buffer`

```cpp
bool read_from_ring_buffer(Fill* fills, int64_t& fill_count,
                           int64_t& order_id, bool& fulfilled,
                           int64_t& remaining_qty,
                           Op* out_op = nullptr);
```

Tries to drain one ack from the outbound buffer.

- Returns `true` if an ack was available and copied into the caller's buffers.
- Returns `false` if the current slot's `is_ready` is still `false` (engine hasn't published yet).

The caller is responsible for polling. The gateway keeps its own outbound read pointer (`outbound_ringbuffer_read_p`) — incremented only on successful reads.

Ack format conventions today:
- For a NEW: `fulfilled` = whether the order was fully filled; `remaining_qty` = leftover that rested; `fill_count` + `fills[]` = the trades produced.
- For a CANCEL: `fulfilled` = whether the cancel succeeded; `fill_count` = 0; `remaining_qty` = 0.
- For a MODIFY: `fulfilled` = whether the modify succeeded; `fill_count` = 0; `remaining_qty` = 0.

If the caller passes a non-null `out_op`, it will be set to the op (`Op::NEW`, `Op::CANCEL`, `Op::MODIFY`) that produced this ack. This lets generic ack handlers branch on op type without remembering what they sent. The `fulfilled` field still carries the per-op meaning above — the op tag just disambiguates which meaning applies.

## Two ways to consume acks

`read_from_ring_buffer` is a single non-blocking poll. The gateway itself doesn't pull acks on its own — somebody has to call it. There are two patterns for doing so, depending on how slow your downstream logic is.

### Pattern 1: caller-drains-directly

The simplest setup. Whoever owns the gateway calls `read_from_ring_buffer` in their own loop:

```cpp
Fill fills[16];
int64_t fill_count, order_id, remaining_qty;
bool fulfilled;
Op op;

while (running) {
    if (gateway.read_from_ring_buffer(fills, fill_count, order_id, fulfilled,
                                      remaining_qty, &op)) {
        // do something with the ack
    }
}
```

Works fine if the consumer can keep up with the engine. Breaks down the moment the consumer is slow — log writes, database persists, network sends, strategy logic — because the outbound ring buffer has no producer-side backpressure (see "Outbound overflow" below). Once the engine's write pointer laps the unread tail, old acks are silently overwritten.

### Pattern 2: shipper thread (`GatewayReader` + `SpscQueue<Ack>`)

For consumers that can't drain at engine speed, insert a thin "shipper" thread between the outbound ring buffer and the consumer:

```
engine ──> outbound ring (fast, small) ──> GatewayReader thread ──> SpscQueue<Ack> ──> consumer
```

The `GatewayReader` (declared in `gateway_reader.hpp`) owns a thread whose only job is to spin on `read_from_ring_buffer` and push each ack into a caller-supplied `SpscQueue<Ack>`. The consumer pops from the queue at whatever rate it likes.

```cpp
SpscQueue<Ack> queue(8192);                       // capacity per gateway
GatewayReader reader(&gateway, &queue);           // starts the thread

// consumer thread (or main loop):
Ack ack;
while (queue.try_pop(ack)) {
    // slow work — log, persist, ship, run strategy logic
}
```

This decouples the engine's write rate from the consumer's processing rate. The ring buffer only needs to absorb the variance of the reader thread (which is fast — just memcpy + push). The queue absorbs the variance of the consumer (which can be slow). Reader's destructor joins the thread cleanly.

#### Why `Ack` instead of `Orderevent_outbound`

`Orderevent_outbound` contains a `std::atomic<bool> is_ready`, and atomics aren't copyable. You can't put one inside a queue's storage. `Ack` is a plain copyable struct mirroring the ack's data fields (`op`, `order_id`, `fulfilled`, `remaining_qty`, `fill_count`, `fills[16]`) — the reader reassembles each outbound slot into an `Ack` before pushing.

#### Overflow policy

If the consumer is too slow even for the queue, `GatewayReader::run` calls `queue.try_push(ack)` and on failure increments `dropped_count_`. Engine never blocks, reader never blocks, the consumer can read `reader.dropped_count()` to see how many acks were lost. The policy is fixed at "drop newest" today — change the `try_push` branch in `gateway_reader.cpp` if you need drop-oldest or blocking semantics.

#### One reader at a time

`GatewayReader` holds the gateway's outbound read pointer through `read_from_ring_buffer`. If your consumer *also* calls `gateway.read_from_ring_buffer` directly, the two race on that pointer and acks get lost or doubled. Pick one pattern per gateway.

## Outbound overflow

The outbound ring buffer is **not backpressured on the writer side**. The engine writes acks unconditionally — it does *not* check `is_ready` before overwriting a slot. The flag is there for the reader's benefit only.

Why this asymmetry: the engine is single-threaded and serves all gateways. If it spin-waited for one slow consumer, every other gateway's events would also stall (head-of-line blocking). Faster to take the loss than to freeze everyone.

Consequence: if the engine's write pointer for a gateway gets `> size` slots ahead of that gateway's read pointer, the next write wraps around and clobbers an unread ack. There is no error, no log, just silently wrong data.

Mitigations:
- **Size the buffer generously.** Memory is cheap; lost acks are not. Doubling `RingBuffer_outbound`'s capacity costs `size × sizeof(Orderevent_outbound)` per gateway.
- **Use the `GatewayReader` shipper-thread pattern** (above) to keep the ring drained at engine speed regardless of downstream pace.
- **Use silent mode** (default for REPLAY gateways — see "Silent flag" above). LOBSTER replay produces millions of acks the validation harness doesn't read; silent mode skips the outbound write entirely and sidesteps the overflow problem rather than mitigating it.

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
