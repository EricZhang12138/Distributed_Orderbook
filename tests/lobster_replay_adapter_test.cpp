#include <gtest/gtest.h>
#include "gateway.hpp"
#include "lobster_replay_adapter.hpp"
#include "ring_buffer.hpp"
#include <sstream>

// These tests verify the parsing and dispatch behavior of LobsterReplayAdapter
// in isolation. We use a REPLAY gateway pointing at a real inbound ring buffer
// but NO matching engine — we then inspect the inbound slots directly to
// confirm the adapter wrote what we expected. This decouples adapter tests
// from engine correctness.

namespace {
struct Fixture {
    RingBuffer_inbound inbound;
    RingBuffer_outbound outbound;
    Gateway gw;
    LobsterReplayAdapter adapter;

    Fixture()
        : inbound(64),
          outbound(64),
          // silent=false override: not strictly needed (no engine to write
          // outbound), but keeps the gateway's behavior identical to LIVE so
          // we can inspect inbound slots without worrying about the silent bit.
          gw(&inbound, &outbound, /*id=*/0, GatewayMode::REPLAY, /*silent_override=*/false),
          adapter(&gw) {}
};
} // namespace

TEST(LobsterReplayAdapter, ParsesAllSupportedTypes) {
    Fixture f;
    // One row of each: 1 (NEW), 2 (PARTIAL), 3 (CANCEL), 4 (EXEC — skip), 99 (garbage — fail).
    // Type 2 needs a prior NEW for the same order_id; we use id=100 for both.
    std::istringstream input(
        "34200.0,1,100,50,5853300,1\n"           // NEW
        "34200.1,2,100,20,5853300,1\n"           // PARTIAL CANCEL (delta 20)
        "34200.2,3,100,0,5853300,1\n"            // CANCEL
        "34200.3,4,200,10,5853300,-1\n"          // EXEC — skipped
        "34200.4,99,300,10,5853300,1\n"          // unknown type — failed
    );
    f.adapter.process_stream(input);

    EXPECT_EQ(f.adapter.rows_processed(), 3);   // 1, 2, 3
    EXPECT_EQ(f.adapter.rows_skipped(),   1);   // 4
    EXPECT_EQ(f.adapter.rows_failed(),    1);   // 99
}

TEST(LobsterReplayAdapter, Type2ConvertsDeltaToNewSize) {
    Fixture f;
    // place 100 shares, then "partial cancel of 30" → new size should be 70.
    std::istringstream input(
        "34200.0,1,42,100,5000000,1\n"
        "34200.1,2,42,30,5000000,1\n"
    );
    f.adapter.process_stream(input);
    ASSERT_EQ(f.adapter.rows_processed(), 2);

    // Slot 0: the NEW. Slot 1: the MODIFY with volume=70 (new size).
    Orderevent_inbound* new_slot = f.inbound.get(0);
    EXPECT_EQ(new_slot->op, Op::NEW);
    EXPECT_EQ(new_slot->internal_order_id, 42);
    EXPECT_EQ(new_slot->volume, 100);
    EXPECT_EQ(new_slot->price, 5000000);

    Orderevent_inbound* mod_slot = f.inbound.get(1);
    EXPECT_EQ(mod_slot->op, Op::MODIFY);
    EXPECT_EQ(mod_slot->internal_order_id, 42);
    EXPECT_EQ(mod_slot->volume, 70);   // 100 - 30, NOT the raw delta of 30
}

TEST(LobsterReplayAdapter, Type2OnUnknownOrderIsFailed) {
    Fixture f;
    // Partial cancel referencing an order the adapter never saw.
    std::istringstream input("34200.0,2,999,10,5000000,1\n");
    f.adapter.process_stream(input);
    EXPECT_EQ(f.adapter.rows_processed(), 0);
    EXPECT_EQ(f.adapter.rows_failed(),    1);
}

TEST(LobsterReplayAdapter, MalformedRowsCountAsFailedAndDoNotAbort) {
    Fixture f;
    std::istringstream input(
        "garbage line\n"
        "34200.0,1,1,10,1000,1\n"     // valid
        "1,2,3\n"                      // too few fields
        "34200.1,3,1,0,1000,1\n"      // valid
    );
    f.adapter.process_stream(input);
    EXPECT_EQ(f.adapter.rows_processed(), 2);  // the two valid rows
    EXPECT_EQ(f.adapter.rows_failed(),    2);  // garbage + too-few-fields
}

TEST(LobsterReplayAdapter, LobsterDirectionMapsToSideCorrectly) {
    Fixture f;
    // LOBSTER direction:  1 = buy  → our side = false (bid tree)
    //                    -1 = sell → our side = true  (ask tree)
    std::istringstream input(
        "34200.0,1,1,10,1000,1\n"     // buy
        "34200.1,1,2,10,1000,-1\n"    // sell
    );
    f.adapter.process_stream(input);
    ASSERT_EQ(f.adapter.rows_processed(), 2);

    EXPECT_FALSE(f.inbound.get(0)->side);   // buy → false
    EXPECT_TRUE (f.inbound.get(1)->side);   // sell → true
}

TEST(LobsterReplayAdapter, EmptyStreamProducesNothing) {
    Fixture f;
    std::istringstream input("");
    f.adapter.process_stream(input);
    EXPECT_EQ(f.adapter.rows_processed(), 0);
    EXPECT_EQ(f.adapter.rows_skipped(),   0);
    EXPECT_EQ(f.adapter.rows_failed(),    0);
}

TEST(LobsterReplayAdapter, EmptyLinesAreIgnored) {
    Fixture f;
    std::istringstream input(
        "\n"
        "34200.0,1,1,10,1000,1\n"
        "\n"
        "\n"
        "34200.1,3,1,0,1000,1\n"
    );
    f.adapter.process_stream(input);
    EXPECT_EQ(f.adapter.rows_processed(), 2);
    EXPECT_EQ(f.adapter.rows_failed(),    0);   // empty lines do NOT count as failures
}

TEST(LobsterReplayAdapter, ProcessFileReturnsFalseForMissingFile) {
    Fixture f;
    EXPECT_FALSE(f.adapter.process_file("/nonexistent/path/that/does/not/exist.csv"));
}
