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
