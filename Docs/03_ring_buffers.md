# Ring Buffers

The ring buffer is the channel between gateways and the matching engine. It's the part of the system that has to be both correct under concurrency and fast — every order pays the cost of crossing it twice (once in, once out).

There are two flavors:

| Buffer | Producers | Consumers | Per-event lock? |
|---|---|---|---|
| `RingBuffer_inbound` | many gateways | one engine | none |
| `RingBuffer_outbound` | one engine | one gateway | none |

## The slot ownership protocol

A ring buffer is a fixed-size circular array. Each slot is owned by exactly one party at any moment. The protocol moves ownership without using locks.

For the **inbound** path:

```
   Gateway                          Engine
   ───────                          ───────
   1. slot = claim()                 (polling slot N)
      → atomic fetch_add returns
        a unique slot index
   2. ord = get(slot)                ...
   3. fill in fields                 ...
   4. publish(slot)                  ...
      → is_ready = true              5. sees is_ready == true (acquire)
                                     6. reads the order data
                                     7. processes it
                                     8. release(slot)
                                        → is_ready = false
                                     9. read_p++
```

Each step is intentional. Walking through the why:

### Step 1 — `claim()` is the only place producers synchronize

```cpp
int64_t RingBuffer_inbound::claim(){
    return write_pointer.fetch_add(1, std::memory_order_relaxed);
}
```

`fetch_add` is a single atomic instruction. Two gateways calling `claim()` at the same instant get two different return values — one gets N, the other N+1. Neither blocks. There is no mutex, no compare-and-swap loop.

`memory_order_relaxed` is enough because `claim` only assigns *which* slot you own; the actual handoff of data happens later at `publish` with stronger ordering.

### Steps 2–3 — Filling the slot is unsynchronized

Once you own slot N, you can write to it freely. No other gateway can touch slot N because each `fetch_add` returns a unique number. The engine isn't reading slot N yet because `is_ready` is still false.

### Step 4 — `publish()` is the synchronizing release

```cpp
void RingBuffer_inbound::publish(int64_t write_p){
    buffer[write_p & mask].is_ready.store(true, std::memory_order_release);
}
```

`std::memory_order_release` is the key. It guarantees that all the writes you did in steps 2–3 (price, volume, side, …) are visible to any thread that subsequently observes `is_ready == true` with `acquire`.

Without release/acquire semantics, the CPU and compiler are free to reorder the writes — the engine could see `is_ready = true` *before* the order data has actually landed in memory. That would be a data race.

### Step 5 — Engine polls with acquire

```cpp
if (event->is_ready.load(std::memory_order_acquire)){ ... }
```

The `acquire` here pairs with the producer's `release`. The two together form a *synchronizes-with* relationship: every write that happened before the `release` is guaranteed visible after the matching `acquire`. The engine can now safely read price, volume, etc., without atomics.

### Step 8 — `release()` returns the slot

```cpp
void release(int64_t write_p){
    buffer[write_p & mask].is_ready.store(false, std::memory_order_relaxed);
}
```

Resets the flag so the slot is reusable on the next wrap-around. Relaxed is fine because the engine is single-threaded; no other reader cares about ordering relative to this store.

## Why the outbound buffer doesn't need an atomic write pointer

The inbound buffer is **multi-producer**: many gateways might call `claim()` concurrently, so the write pointer must be atomic to hand out unique slot numbers.

The outbound buffer is **single-producer**: only the engine writes to it. The engine keeps its outbound write pointers in `std::vector<int64_t> write_p_outbound;` — plain `int64_t`, no atomics. Same reason it's per-gateway: one engine writing into one buffer means no contention.

The outbound buffer is also **single-consumer**: only the owning gateway reads it. That makes it a classic SPSC (single-producer, single-consumer) channel, which is the easiest concurrency primitive to reason about.

## Performance tricks

### Power-of-two size + bitmask indexing

```cpp
RingBuffer_inbound(int64_t buffer_size)
    : size(buffer_size), mask(buffer_size - 1) {}
// ...
buffer[write_p & mask]
```

`write_p & mask` is equivalent to `write_p % size` when `size` is a power of two. The bitmask version is a single AND instruction; modulo on a non-constant divisor is a 20–40 cycle division. This is one of the standard Disruptor optimizations.

The constraint: callers must construct the buffer with a power-of-two size (e.g. 1024). There's no runtime check for this today.

### Cache-line alignment

```cpp
struct alignas(64) Orderevent_inbound { ... };
struct alignas(64) Orderevent_outbound { ... };
```

A modern x86 cache line is 64 bytes. Aligning the event to a cache-line boundary means one event = one (or a small number of) line(s), and adjacent events live in adjacent lines. This:
- Avoids one event straddling two cache lines (which would cost an extra fetch on every access).
- Reduces false sharing — two threads working on adjacent events don't accidentally share a cache line.

### Storing events by value, not by pointer

```cpp
std::unique_ptr<Orderevent_inbound[]> buffer;
```

The buffer holds the events directly, not pointers to them. The whole array is one contiguous heap allocation. When the engine polls slot N, it touches the bytes of that event — no extra pointer chase to a separately-allocated object.

(Side note: the `unique_ptr<T[]>` here is just to handle heap allocation cleanly; the elements are by-value within the array.)

### Why `is_ready` is `std::atomic<bool>` and not, say, a separate flag array

Keeping the flag inside the event struct means the producer and consumer touch the same cache line for both the flag and the payload. That makes the acquire/release pairing self-contained per slot. The cost is the false-sharing risk noted below.

## Known issues / where this design pays a cost

### `is_ready` shares a cache line with the payload

The struct is 64-byte aligned but `is_ready` sits at the end of it, on the same cache line as price/volume/etc. While the producer is writing the payload, the consumer is polling `is_ready`, so the line ping-pongs between cores. The classic Disruptor design puts the ready flag on its own line.

### No backpressure check

`claim()` does an unconditional `fetch_add`. If gateways produce faster than the engine consumes, the write pointer will lap the buffer and overwrite slots whose `is_ready` is still `true` — silently corrupting in-flight orders.

A correct implementation would, before claiming, verify that `write_p - cached_read_p < size`. The engine would publish its `read_p` atomically (relaxed is fine — we're only checking a bound), and the gateway would consult a cached copy of it.

This is fine for the current dev workload but will be a real problem under data-replay load.

### Outbound buffer has the same backpressure gap

The engine writes outbound slots without checking whether the gateway has caught up. If a gateway stops reading, its outbound buffer wraps and overwrites unread acks. Symmetric fix.

## Glossary

- **Producer** — thread that writes events into the buffer (gateway, for inbound; engine, for outbound).
- **Consumer** — thread that reads events (engine, for inbound; gateway, for outbound).
- **MPSC / SPSC** — multi-producer/single-consumer, single-producer/single-consumer.
- **Memory order release** — "all my prior writes are now visible to anyone who does an acquire on the same atomic."
- **Memory order acquire** — "I will see all writes that happened before any release on this atomic."
- **False sharing** — two threads modifying different variables that happen to live on the same cache line, causing unnecessary cache-line invalidations.
