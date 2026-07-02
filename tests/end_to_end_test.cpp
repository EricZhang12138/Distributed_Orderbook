#include <gtest/gtest.h>
#include "gateway.hpp"
#include "matching_engine.hpp"
#include "ring_buffer.hpp"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// End-to-end tests spin up a matching engine thread and one or more gateways,
// then verify that orders placed via gateways produce the correct fills on the
// outbound ring buffer.
//
// NOTE: MatchingEngine::run() is currently an infinite loop with no stop flag,
// so we cannot cleanly join the engine thread at end of test. Instead, each
// test allocates its EngineHarness on the heap and intentionally leaks it —
// the detached engine thread then keeps reading valid memory until the test
// process exits. Destroying the harness while the thread is still running
// would leave the detached thread touching freed memory, which corrupts
// subsequent tests.

namespace {

// spin-wait for a response on the given gateway, with a timeout
bool wait_for_response(Gateway& gw,
                       Fill* fills,
                       int64_t& fill_count,
                       int64_t& order_id,
                       bool& fulfilled,
                       int64_t& remaining_qty,
                       int timeout_ms = 1000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (gw.read_from_ring_buffer(fills, fill_count, order_id, fulfilled, remaining_qty)) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

struct EngineHarness {
    RingBuffer_inbound inbound;
    std::vector<std::unique_ptr<RingBuffer_outbound>> outbounds;
    std::vector<RingBuffer_outbound*> outbound_ptrs;
    std::unique_ptr<MatchingEngine> engine;
    std::thread engine_thread;
    std::vector<std::unique_ptr<Gateway>> gateways;

    EngineHarness(int num_gateways, int64_t buffer_size = 1024)
        : inbound(buffer_size) {
        for (int i = 0; i < num_gateways; i++) {
            outbounds.push_back(std::make_unique<RingBuffer_outbound>(buffer_size));
        }
        for (auto& ob : outbounds) outbound_ptrs.push_back(ob.get());

        engine = std::make_unique<MatchingEngine>(&inbound, outbound_ptrs);

        for (int i = 0; i < num_gateways; i++) {
            gateways.push_back(std::make_unique<Gateway>(&inbound, outbounds[i].get(), i));
        }
        engine_thread = std::thread([this] { engine->run(); });
    }

    ~EngineHarness() {
        // MatchingEngine::run() has no stop flag, detach so ~thread() does not
        // terminate the process.
        if (engine_thread.joinable()) engine_thread.detach();
    }
};

// Allocate a harness on the heap and intentionally leak it. See top-of-file
// comment for why. Returns a reference for ergonomic use at the call site.
EngineHarness& make_harness(int num_gateways, int64_t buffer_size = 1024) {
    return *(new EngineHarness(num_gateways, buffer_size));
}

} // namespace

TEST(EndToEnd, SingleGatewayPostsRestingSell) {
    EngineHarness& h = make_harness(1);

    h.gateways[0]->place_order_to_ring_buffer(100, 10, true, "alice");

    Fill fills[16];
    int64_t fill_count = 0, order_id = 0, remaining_qty = 0;
    bool fulfilled = false;
    ASSERT_TRUE(wait_for_response(*h.gateways[0], fills, fill_count, order_id, fulfilled, remaining_qty));
    EXPECT_FALSE(fulfilled);
    EXPECT_EQ(remaining_qty, 10);
    EXPECT_EQ(fill_count, 0);
}

TEST(EndToEnd, TwoGatewaysCross_FillsRoutedToBothSides) {
    EngineHarness& h = make_harness(2);

    // gateway 0 posts a sell, gateway 1 posts a crossing buy
    h.gateways[0]->place_order_to_ring_buffer(100, 10, true,  "alice");
    h.gateways[1]->place_order_to_ring_buffer(100, 10, false, "bob");

    Fill fills[16];
    int64_t fill_count = 0, order_id = 0, remaining_qty = 0;
    bool fulfilled = false;

    // gateway 0 should see the resting sell acknowledgement
    ASSERT_TRUE(wait_for_response(*h.gateways[0], fills, fill_count, order_id, fulfilled, remaining_qty));
    EXPECT_FALSE(fulfilled);
    EXPECT_EQ(remaining_qty, 10);

    // gateway 1 should see a full fill
    ASSERT_TRUE(wait_for_response(*h.gateways[1], fills, fill_count, order_id, fulfilled, remaining_qty));
    EXPECT_TRUE(fulfilled);
    EXPECT_EQ(remaining_qty, 0);
    ASSERT_EQ(fill_count, 1);
    EXPECT_EQ(fills[0].price, 100);
    EXPECT_EQ(fills[0].volume, 10);
}

TEST(EndToEnd, CrossingBuySweepsMultipleAsks) {
    EngineHarness& h = make_harness(2);

    h.gateways[0]->place_order_to_ring_buffer(100, 5, true, "a");
    h.gateways[0]->place_order_to_ring_buffer(101, 5, true, "a");
    h.gateways[0]->place_order_to_ring_buffer(102, 5, true, "a");

    // drain gateway 0's three rest acks
    Fill fills[16];
    int64_t fill_count = 0, order_id = 0, remaining_qty = 0;
    bool fulfilled = false;
    for (int i = 0; i < 3; i++) {
        ASSERT_TRUE(wait_for_response(*h.gateways[0], fills, fill_count, order_id, fulfilled, remaining_qty));
    }

    // gateway 1 sweeps with a big buy
    h.gateways[1]->place_order_to_ring_buffer(102, 12, false, "b");
    ASSERT_TRUE(wait_for_response(*h.gateways[1], fills, fill_count, order_id, fulfilled, remaining_qty));
    EXPECT_TRUE(fulfilled);
    EXPECT_EQ(remaining_qty, 0);
    ASSERT_EQ(fill_count, 3);
    EXPECT_EQ(fills[0].price, 100);
    EXPECT_EQ(fills[1].price, 101);
    EXPECT_EQ(fills[2].price, 102);
}

TEST(EndToEnd, CancelRemovesRestingOrder) {
    EngineHarness& h = make_harness(2);

    Fill fills[16];
    int64_t fill_count = 0, order_id = 0, remaining_qty = 0;
    bool fulfilled = false;

    // gateway 0 posts a resting sell at price 100.
    int64_t sell_id = h.gateways[0]->place_order_to_ring_buffer(100, 10, true, "alice");
    ASSERT_TRUE(wait_for_response(*h.gateways[0], fills, fill_count, order_id, fulfilled, remaining_qty));
    EXPECT_FALSE(fulfilled);
    EXPECT_EQ(remaining_qty, 10);

    // cancel it; expect success ack and no fills.
    h.gateways[0]->cancel_order_to_ring_buffer(sell_id);
    ASSERT_TRUE(wait_for_response(*h.gateways[0], fills, fill_count, order_id, fulfilled, remaining_qty));
    EXPECT_EQ(order_id, sell_id);
    EXPECT_TRUE(fulfilled);              // cancel reuses 'fulfilled' as the success flag
    EXPECT_EQ(fill_count, 0);

    // re-cancelling the same id must fail — the order is gone from global_map.
    h.gateways[0]->cancel_order_to_ring_buffer(sell_id);
    ASSERT_TRUE(wait_for_response(*h.gateways[0], fills, fill_count, order_id, fulfilled, remaining_qty));
    EXPECT_FALSE(fulfilled);

    // proof the resting sell really left the book: a buy at 100 should now rest
    // rather than crossing.
    h.gateways[1]->place_order_to_ring_buffer(100, 10, false, "bob");
    ASSERT_TRUE(wait_for_response(*h.gateways[1], fills, fill_count, order_id, fulfilled, remaining_qty));
    EXPECT_FALSE(fulfilled);
    EXPECT_EQ(remaining_qty, 10);
    EXPECT_EQ(fill_count, 0);
}

TEST(EndToEnd, ModifyReducesRestingSize) {
    EngineHarness& h = make_harness(2);

    Fill fills[16];
    int64_t fill_count = 0, order_id = 0, remaining_qty = 0;
    bool fulfilled = false;

    // gateway 0 posts a resting sell of 10 at price 100.
    int64_t sell_id = h.gateways[0]->place_order_to_ring_buffer(100, 10, true, "alice");
    ASSERT_TRUE(wait_for_response(*h.gateways[0], fills, fill_count, order_id, fulfilled, remaining_qty));
    EXPECT_EQ(remaining_qty, 10);

    // shrink it to 4.
    h.gateways[0]->modify_order_to_ring_buffer(sell_id, 4);
    ASSERT_TRUE(wait_for_response(*h.gateways[0], fills, fill_count, order_id, fulfilled, remaining_qty));
    EXPECT_EQ(order_id, sell_id);
    EXPECT_TRUE(fulfilled);
    EXPECT_EQ(fill_count, 0);

    // a crossing buy of 10 must now fill only 4 — the remaining 6 rests as a bid.
    h.gateways[1]->place_order_to_ring_buffer(100, 10, false, "bob");
    ASSERT_TRUE(wait_for_response(*h.gateways[1], fills, fill_count, order_id, fulfilled, remaining_qty));
    EXPECT_FALSE(fulfilled);             // not fully filled, 6 left over
    EXPECT_EQ(remaining_qty, 6);
    ASSERT_EQ(fill_count, 1);
    EXPECT_EQ(fills[0].price, 100);
    EXPECT_EQ(fills[0].volume, 4);       // proves the modify took effect
}

TEST(EndToEnd, ModifyRejectsSizeUp) {
    EngineHarness& h = make_harness(2);

    Fill fills[16];
    int64_t fill_count = 0, order_id = 0, remaining_qty = 0;
    bool fulfilled = false;

    // gateway 0 posts a resting sell of 5 at price 100.
    int64_t sell_id = h.gateways[0]->place_order_to_ring_buffer(100, 5, true, "alice");
    ASSERT_TRUE(wait_for_response(*h.gateways[0], fills, fill_count, order_id, fulfilled, remaining_qty));
    EXPECT_EQ(remaining_qty, 5);

    // attempt to grow it to 100 — should be rejected.
    h.gateways[0]->modify_order_to_ring_buffer(sell_id, 100);
    ASSERT_TRUE(wait_for_response(*h.gateways[0], fills, fill_count, order_id, fulfilled, remaining_qty));
    EXPECT_EQ(order_id, sell_id);
    EXPECT_FALSE(fulfilled);             // rejected
    EXPECT_EQ(fill_count, 0);

    // confirm the order is still 5: a crossing buy of 100 should fill exactly 5 and rest with 95.
    h.gateways[1]->place_order_to_ring_buffer(100, 100, false, "bob");
    ASSERT_TRUE(wait_for_response(*h.gateways[1], fills, fill_count, order_id, fulfilled, remaining_qty));
    EXPECT_FALSE(fulfilled);
    EXPECT_EQ(remaining_qty, 95);
    ASSERT_EQ(fill_count, 1);
    EXPECT_EQ(fills[0].volume, 5);       // original size, not the rejected new size
}

TEST(EndToEnd, SilentReplayGatewayProducesNoOutboundAcks) {
    // Harness creates 2 LIVE gateways by default. Replace gateway 1 with a REPLAY
    // gateway (defaults to silent=true). The engine doesn't care about gateway
    // identity — it just routes by gateway_id — so swapping is safe.
    EngineHarness& h = make_harness(2);
    h.gateways[1] = std::make_unique<Gateway>(&h.inbound, h.outbounds[1].get(), 1, GatewayMode::REPLAY);
    ASSERT_FALSE(h.gateways[0]->is_silent());
    ASSERT_TRUE (h.gateways[1]->is_silent());

    // Silent REPLAY gateway places a resting sell. Engine should update the book but write no ack.
    h.gateways[1]->place_order_to_ring_buffer(100, 5, true, "feed", /*external_id=*/1001, /*external_ts=*/12345);

    // LIVE gateway sends a crossing buy. It should receive an ack with one fill
    // against the replay-resting sell — proving the silent event was actually processed.
    Fill fills[16];
    int64_t fill_count = 0, order_id = 0, remaining_qty = 0;
    bool fulfilled = false;
    h.gateways[0]->place_order_to_ring_buffer(100, 5, false, "alice");
    ASSERT_TRUE(wait_for_response(*h.gateways[0], fills, fill_count, order_id, fulfilled, remaining_qty));
    EXPECT_TRUE(fulfilled);
    ASSERT_EQ(fill_count, 1);
    EXPECT_EQ(fills[0].price, 100);
    EXPECT_EQ(fills[0].volume, 5);

    // Gateway 1's outbound buffer should never have had is_ready flipped on.
    // Give the engine a moment to ensure nothing arrives late.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(h.outbounds[1]->get(0)->is_ready.load());
}

TEST(EndToEnd, ConcurrentPlacements_AllOrdersProcessed) {
    constexpr int kNumGateways = 3;
    constexpr int kOrdersPerGateway = 50;
    EngineHarness& h = make_harness(kNumGateways);

    std::vector<std::thread> producers;
    for (int g = 0; g < kNumGateways; g++) {
        producers.emplace_back([&h, g] {
            for (int i = 0; i < kOrdersPerGateway; i++) {
                // mix of resting orders that won't cross (far-apart prices)
                int64_t price = 1000 + g * 100 + i; // unique per (gateway, i)
                bool side = (i % 2 == 0);
                h.gateways[g]->place_order_to_ring_buffer(price, 1, side, "user");
            }
        });
    }
    for (auto& t : producers) t.join();

    // each gateway should eventually see kOrdersPerGateway responses back
    for (int g = 0; g < kNumGateways; g++) {
        int received = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        Fill fills[16];
        int64_t fill_count = 0, order_id = 0, remaining_qty = 0;
        bool fulfilled = false;
        while (received < kOrdersPerGateway &&
               std::chrono::steady_clock::now() < deadline) {
            if (h.gateways[g]->read_from_ring_buffer(fills, fill_count, order_id, fulfilled, remaining_qty)) {
                received++;
            } else {
                std::this_thread::yield();
            }
        }
        EXPECT_EQ(received, kOrdersPerGateway) << "gateway " << g;
    }
}
