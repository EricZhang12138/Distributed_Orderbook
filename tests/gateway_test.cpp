#include <gtest/gtest.h>
#include "gateway.hpp"
#include "ring_buffer.hpp"

// These tests exercise Gateway in isolation — no matching engine thread.
// They verify that place_order_to_ring_buffer writes into the inbound buffer
// correctly, and that read_from_ring_buffer correctly consumes a ready outbound
// slot.

TEST(GatewayTest, PlaceOrderWritesIntoInboundBuffer) {
    RingBuffer_inbound inbound(8);
    RingBuffer_outbound outbound(8);
    Gateway gw(&inbound, &outbound, 7);

    gw.place_order_to_ring_buffer(100, 10, true, "alice");

    Orderevent_inbound* slot = inbound.get(0);
    EXPECT_EQ(slot->price, 100);
    EXPECT_EQ(slot->volume, 10);
    EXPECT_TRUE(slot->side);
    EXPECT_EQ(slot->gateway_id, 7);
    EXPECT_TRUE(slot->is_ready.load());
}

TEST(GatewayTest, MultiplePlaceOrdersAdvanceWritePointer) {
    RingBuffer_inbound inbound(8);
    RingBuffer_outbound outbound(8);
    Gateway gw(&inbound, &outbound, 0);

    gw.place_order_to_ring_buffer(100, 1, true, "a");
    gw.place_order_to_ring_buffer(101, 2, false, "a");
    gw.place_order_to_ring_buffer(102, 3, true, "a");

    EXPECT_EQ(inbound.get(0)->price, 100);
    EXPECT_EQ(inbound.get(1)->price, 101);
    EXPECT_EQ(inbound.get(2)->price, 102);
}

TEST(GatewayTest, ReadFromRingBufferReturnsFalseWhenEmpty) {
    RingBuffer_inbound inbound(8);
    RingBuffer_outbound outbound(8);
    Gateway gw(&inbound, &outbound, 0);

    Fill fills[16];
    int64_t fill_count = 0, order_id = 0, remaining_qty = 0;
    bool fulfilled = false;
    EXPECT_FALSE(gw.read_from_ring_buffer(fills, fill_count, order_id, fulfilled, remaining_qty));
}

TEST(GatewayTest, ReadFromRingBufferConsumesReadySlot) {
    RingBuffer_inbound inbound(8);
    RingBuffer_outbound outbound(8);
    Gateway gw(&inbound, &outbound, 0);

    // manually populate the outbound slot as if the engine had written to it
    Orderevent_outbound* slot = outbound.get(0);
    slot->order_id = 42;
    slot->gateway_id = 0;
    slot->fulfilled = true;
    slot->remaining_qty = 0;
    slot->fill_count = 2;
    slot->fills[0] = {100, 4, 1000};
    slot->fills[1] = {101, 6, 2000};
    outbound.publish(0);

    Fill fills[16];
    int64_t fill_count = 0, order_id = 0, remaining_qty = 0;
    bool fulfilled = false;
    EXPECT_TRUE(gw.read_from_ring_buffer(fills, fill_count, order_id, fulfilled, remaining_qty));
    EXPECT_EQ(order_id, 42);
    EXPECT_TRUE(fulfilled);
    EXPECT_EQ(remaining_qty, 0);
    ASSERT_EQ(fill_count, 2);
    EXPECT_EQ(fills[0].price, 100);
    EXPECT_EQ(fills[0].volume, 4);
    EXPECT_EQ(fills[1].price, 101);
    EXPECT_EQ(fills[1].volume, 6);

    // after consuming, a second read should return false since is_ready was cleared
    EXPECT_FALSE(gw.read_from_ring_buffer(fills, fill_count, order_id, fulfilled, remaining_qty));
}
