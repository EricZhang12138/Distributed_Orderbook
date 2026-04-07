# Tests

This directory contains the test suite for the orderbook project. Tests are written using [Google Test](https://github.com/google/googletest), which is fetched automatically by CMake — no manual install required.

## Structure

The tests are organised into four files, each targeting a different layer of the system:

```
tests/
├── orderbook_test.cpp     # Pure unit tests for the matching logic
├── ring_buffer_test.cpp   # Unit tests for the inbound/outbound ring buffers
├── gateway_test.cpp       # Unit tests for the Gateway in isolation
└── end_to_end_test.cpp    # Integration tests with a real engine thread
```

The layering goes from cheapest/fastest at the top (pure logic, no threads) to slowest at the bottom (multithreaded round-trip through the full pipeline). When debugging, prefer running the lower layers first — a failure there usually explains failures in the higher layers.

## How to build and run

From the project root:

```bash
cd build
cmake ..
make orderbook_tests
./orderbook_tests
```

Filter to a subset by test suite or name:

```bash
./orderbook_tests --gtest_filter='OrderbookTest.*'
./orderbook_tests --gtest_filter='*Cancel*'
./orderbook_tests --gtest_filter='-EndToEnd.*'   # exclude end-to-end
```

CMake also registers the tests with CTest, so you can run:

```bash
ctest --output-on-failure
```

## What each file covers

### `orderbook_test.cpp` — `Orderbook` matching logic

Pure unit tests against `Orderbook::placeOrder` and `Orderbook::cancel`. No threads, no ring buffers, no gateway. Each test constructs a fresh `Orderbook` and exercises one piece of behaviour.

| Test | What it verifies |
|---|---|
| `SellRestsWhenNoBids` | A sell with no opposing bids rests on the book |
| `BuyRestsWhenNoAsks` | A buy with no opposing asks rests on the book |
| `BuyExactlyFillsSingleSell` | Equal-volume cross fully fills both sides |
| `SellExactlyFillsSingleBuy` | Same as above, opposite direction |
| `BuyPartiallyFillsSell_RestRemains` | Partial fill leaves the resting side intact |
| `BuyLargerThanSell_LeftoverRestsInBook` | Excess crosses into a new resting order |
| `BuySweepsMultipleAskLevels_LowestFirst` | Price priority — lowest ask matched first |
| `SellSweepsMultipleBidLevels_HighestFirst` | Price priority — highest bid matched first |
| `FifoWithinPriceLevel` | Time priority — earlier orders at the same price fill first |
| `NonCrossingBuyRests` | A buy below the best ask does not cross |
| `CancelExistingOrder` | A cancelled order is removed and no longer matchable |
| `CancelNonexistentOrderReturnsFalse` | Cancelling an unknown ID returns `false` |
| `CancelHeadOfThreeOrdersSamePrice` | Linked-list head removal |
| `CancelMiddleOfThreeOrdersSamePrice` | Linked-list middle removal |
| `CancelTailOfThreeOrdersSamePrice` | Linked-list tail removal |
| `CancelOnlyOrderPrunesLimit` | Cancelling the last order at a price prunes the `Limit` |
| `CancelTwiceReturnsFalseSecondTime` | Double-cancel is rejected |

### `ring_buffer_test.cpp` — `RingBuffer_inbound` / `RingBuffer_outbound`

Single-threaded tests against the ring buffer primitives. These verify the contract that the gateways and engine rely on.

| Test | What it verifies |
|---|---|
| `RingBufferInbound.ClaimReturnsSequentialIndices` | `claim()` hands out monotonically increasing tickets |
| `RingBufferInbound.GetReturnsDistinctSlotsBeforeWrap` | Different indices map to different slots within one revolution |
| `RingBufferInbound.GetWrapsAroundMask` | Indices `n` and `n + size` map to the same slot |
| `RingBufferInbound.PublishAndReleaseTogglesIsReady` | `publish()` sets `is_ready = true`, `release()` clears it |
| `RingBufferInbound.DataWrittenToSlotPersists` | Writes to a slot are visible on subsequent reads |
| `RingBufferOutbound.GetWrapsAroundMask` | Same wraparound contract on the outbound side |
| `RingBufferOutbound.PublishAndReleaseTogglesIsReady` | Outbound publish/release toggle |
| `RingBufferOutbound.DataWrittenToSlotPersists` | Outbound slot writes persist (including the `Fill` array) |

### `gateway_test.cpp` — `Gateway` in isolation

Tests the `Gateway` class without spinning up the matching engine. These verify the gateway's interaction with the ring buffers, not the matching logic.

| Test | What it verifies |
|---|---|
| `PlaceOrderWritesIntoInboundBuffer` | `place_order_to_ring_buffer` populates the inbound slot and marks it ready |
| `MultiplePlaceOrdersAdvanceWritePointer` | Successive places land in successive slots |
| `ReadFromRingBufferReturnsFalseWhenEmpty` | Reading from an empty outbound buffer returns `false` |
| `ReadFromRingBufferConsumesReadySlot` | A manually-published outbound slot is correctly consumed and `is_ready` is cleared afterwards |

### `end_to_end_test.cpp` — full pipeline integration

These tests spin up a real `MatchingEngine` thread and one or more `Gateway` instances, then verify that orders flow through the inbound buffer, get matched, and produce correct responses on the outbound buffers.

A helper struct `EngineHarness` builds the inbound buffer, the per-gateway outbound buffers, the engine, and the gateways, then starts the engine thread. Because `MatchingEngine::run()` is currently an infinite loop with no stop flag, the harness destructor `detach()`s the engine thread instead of joining it (the gtest process exits between cases, so this is safe in practice).

A helper function `wait_for_response` polls a gateway's outbound buffer with a timeout so tests don't hang forever if a response never arrives.

| Test | What it verifies |
|---|---|
| `SingleGatewayPostsRestingSell` | An order placed via the gateway makes it through the engine and produces a "rests on book" response |
| `TwoGatewaysCross_FillsRoutedToBothSides` | A cross between two gateways routes the resting ack to one and the fill to the other |
| `CrossingBuySweepsMultipleAsks` | A large crossing buy sweeps multiple ask levels and the gateway sees all fills |
| `ConcurrentPlacements_AllOrdersProcessed` | Three gateways concurrently place 50 orders each; every response is eventually delivered to the right gateway |

## Adding new tests

Each test file is a standalone translation unit linked into the `orderbook_tests` binary. To add a test, write a new `TEST(Suite, Name)` block in the appropriate file. To add a brand new file, list it under `add_executable(orderbook_tests ...)` in the top-level `CMakeLists.txt`.

A few conventions used here:

- **One assertion per concept.** Prefer multiple small `EXPECT_*` checks over one giant compound assertion — failure messages are clearer.
- **`ASSERT_*` for preconditions, `EXPECT_*` for actual checks.** `ASSERT_*` halts the test on failure (useful when subsequent code would crash), `EXPECT_*` records the failure but continues.
- **Each `TEST` is independent.** Always construct fresh objects inside the test body — never rely on state from a previous test.
- **End-to-end tests should always have a timeout.** Use `wait_for_response` (or similar) so a broken pipeline produces a test failure, not a hang.
