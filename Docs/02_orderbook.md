# Orderbook Internals

The `Orderbook` is the matching engine's core data structure. It keeps every resting order, finds the best opposite price, walks the time-priority queue at that price, and produces fills.

## Data structures

Three pieces working together:

### 1. Two red-black trees: `bids` and `asks`

```cpp
std::map<int64_t, std::unique_ptr<Limit>> bids;  // price → Limit
std::map<int64_t, std::unique_ptr<Limit>> asks;
```

`std::map` is a red-black tree, so price lookups are O(log L) where L is the number of distinct price levels.

- **Best bid** = `bids.rbegin()` (highest key — the buyer willing to pay the most).
- **Best ask** = `asks.begin()` (lowest key — the seller asking the least).

When a new order arrives, we walk the *opposite* tree from the best side toward worse prices, taking liquidity, until either the new order is fully filled or the next price level no longer crosses.

### 2. `Limit` — head of a doubly-linked list per price level

```cpp
struct Limit {
    Order* head;        // oldest order at this price (FIFO front)
    Order* tail;        // newest order at this price
    int64_t count;      // number of orders
    int64_t totalVolume;// sum of order volumes — useful for L2 snapshots
};
```

One `Limit` per price level. Orders at the same price are kept in time-priority order: `head` is the oldest (gets filled first), `tail` is the newest.

### 3. `Order` — node, with the list pointers *inside* the order itself

```cpp
struct Order {
    int64_t price, volume, orderID;
    bool side;
    int64_t order_arrival_time;
    Order* next;
    Order* prev;
    Limit* parentlimit;   // back-pointer to the Limit this order belongs to
};
```

This is the **intrusive doubly-linked list** pattern. The `next` / `prev` pointers are baked into the `Order` itself — there is no separate "node" wrapper around the `Order`.

### 4. `global_map` — direct lookup by order id

```cpp
std::unordered_map<int64_t, Order*> global_map;
```

Maps `order_id → Order*`. Used exclusively for cancellation: given an id, jump straight to the order, then use the order's `parentlimit` + `prev` / `next` to splice it out in O(1).

## Why this combination is fast

### Intrusive linked list vs `std::list`

`std::list<Order>` would wrap each `Order` in an internal node struct (the node contains the `Order` plus `prev`/`next` plus heap-allocation overhead). Every insert is a heap allocation; every traversal touches two cache lines per element (the node, then the order).

By making the order itself the node, we get:
- One heap allocation per order instead of two (the order's own `new`, no list-node wrapper).
- Cache-friendly traversal: walking the queue at a price level dereferences one pointer per step, into one cache line.
- O(1) cancellation: from the `Order*` (obtained via `global_map`), we have its `prev`, `next`, and `parentlimit` directly.

If we had used `std::list`, cancellation would either require storing iterators (fragile across modifications) or scanning the list (O(N)).

### Back-pointer `parentlimit`

When we cancel an order, we need to update the parent `Limit`'s `count` and `totalVolume`, and possibly remove the Limit from the tree if it becomes empty. Without `parentlimit`, we'd have to either:
- Search the price in the tree (O(log L)), or
- Carry the price → look it up.

The back-pointer makes cancel completely O(1) once we have the `Order*`.

### Why `std::map` rather than a flat array

For a single symbol with a tight tick range, a flat `std::array<Limit, N>` indexed by `(price - min) / tick` is faster — direct array access vs tree descent. Real exchanges do this.

`std::map` was chosen here because:
- Tick size and price range vary by symbol; the flat-array approach requires knowing them ahead of time.
- It is the easier-to-reason-about starting point.

This is the single biggest performance lever to pull later if the engine becomes the bottleneck.

## `placeOrder` / `match` walk-through

`placeOrder` is a thin wrapper around `match`, which holds the actual logic. The function is split into two near-mirror branches: one for sells (cross against bids), one for buys (cross against asks). The sell branch is described below; the buy branch is symmetric.

```
match(price, volume, side=SELL, ...):
    while bids is non-empty AND best_bid.price >= my_price AND volume > 0:
        Limit& level = bids.rbegin()        // highest-priced bid
        while level.count > 0 AND volume > 0:
            Order& maker = *level.head
            if maker.volume > volume:
                # Partial fill of the resting order
                maker.volume -= volume
                level.totalVolume -= volume
                record fill(price=maker.price, volume=volume)
                return DONE
            else:
                # Maker fully consumed — pop the head
                volume -= maker.volume
                level.totalVolume -= maker.volume
                level.head = maker.next
                level.count -= 1
                global_map.erase(maker.id)
                delete &maker
                record fill(...)
                if level.count == 0:
                    bids.erase(this_price)
                    break
    if volume > 0:
        # Did not fully cross — rest the remainder in the asks tree
        rest_in_asks(price, volume, ...)
```

Key correctness points:
- Matching only walks `head` — that is, the oldest order at the best price. Time priority is preserved.
- An empty `Limit` (count = 0) is erased from the tree to avoid stale entries and to keep `rbegin()` / `begin()` meaningful.
- If the order doesn't fully cross, the remainder is inserted into its own side; `global_map` is updated so it can be cancelled later.

## `modify` walk-through

`modify(order_id, new_size)` reduces an existing order's volume in place. The order keeps its position in the doubly-linked list at its price level, so its **queue priority is preserved**.

```
modify(orderID, new_size):
    it = global_map.find(orderID)
    if not found: return false

    order = *it
    if new_size <= 0:                       return false  // use cancel for size→0
    if new_size >= order.volume:            return false  // no size-up via modify

    delta = order.volume - new_size
    order.volume = new_size
    order.parentlimit.totalVolume -= delta
    return true
```

That is the entire implementation. Notice what it does **not** touch:

- `prev` / `next` — the order stays at the same point in the list.
- `parentlimit->count` — there are still the same number of orders at this price.
- `global_map` — the id-to-pointer mapping is unchanged.
- The two trees (`bids`/`asks`) — no Limit is added or removed.

### Why this matters (and why "cancel + new" is wrong)

A naive size reduction would `cancel(order_id)` and then `placeOrder(price, new_size, ...)`. The book's numbers (count, totalVolume, top-of-book) end up identical to the in-place modify. But:

- Cancel + new puts the order at the **tail** of the queue at its price.
- If any other order arrived at the same price between the cancel and the new, that order is now ahead of you.
- Even if no such order arrived, your timestamp is "now" rather than the original arrival time — fairness arguments downstream may differ.

For data replay (LOBSTER type 2 messages), losing queue priority means your matcher's fills will diverge from reality within minutes on a liquid stock. Hence the in-place implementation.

### What MODIFY does *not* support

- **Size up.** Increasing volume is structurally different — it would let a stale order jump ahead of orders that queued behind it. Real exchanges implement size-up as cancel + new at the back of the queue. NASDAQ (and therefore LOBSTER) emits this as a type-3 followed by a type-1, so you never need a "modify-up" path.
- **Price change.** Same reason as size-up: an order at a new price has no natural queue position to inherit. LOBSTER decomposes price changes into type-3 + type-1.

So in practice MODIFY = strict size-down only. The two `return false` checks in the implementation enforce that contract.

## `cancel` walk-through

```
cancel(orderID):
    it = global_map.find(orderID)
    if not found: return false

    order = *it
    splice out of doubly-linked list:
        if order.prev: order.prev.next = order.next
        if order.next: order.next.prev = order.prev
        if order is head of its Limit: Limit.head = order.next
        if order is tail of its Limit: Limit.tail = order.prev

    Limit.count -= 1
    Limit.totalVolume -= order.volume
    global_map.erase(it)
    delete order

    if Limit.count == 0:
        erase the Limit from bids or asks (by side + price)

    return true
```

Worst-case complexity: O(1) for the splice and map removals, plus O(log L) for the optional tree erase. Effectively O(1) in steady state.

## Time priority and FIFO

Within one price level, orders are processed strictly in arrival order. This is guaranteed by:
- New rests always go to `tail`.
- Matching always consumes from `head`.

This is essential for fairness and is the reason an order placed earlier at a given price gets filled before a later one at the same price.

## Known limitations

- `match()` is implemented twice (once per side). A templated implementation would halve the code and remove a class of "fixed one side, forgot the other" bugs.
- Orders are heap-allocated one at a time via `new`. A pool allocator would reduce allocator overhead and improve cache locality.
- `bids`/`asks` are `std::map` — see the flat-array note above.
- `order_arrival_time` passed into `match` is not stored on resting orders (the field exists on `Order` but the `new Order{price, volume, orderID, side}` brace-init at insert time does not include it).
