#include <gtest/gtest.h>
#include "ring_buffer.hpp"

// --- inbound ring buffer ---

TEST(RingBufferInbound, ClaimReturnsSequentialIndices) {
    RingBuffer_inbound rb(8);
    EXPECT_EQ(rb.claim(), 0);
    EXPECT_EQ(rb.claim(), 1);
    EXPECT_EQ(rb.claim(), 2);
}

TEST(RingBufferInbound, GetReturnsDistinctSlotsBeforeWrap) {
    RingBuffer_inbound rb(8);
    Orderevent_inbound* a = rb.get(0);
    Orderevent_inbound* b = rb.get(1);
    Orderevent_inbound* c = rb.get(2);
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    EXPECT_NE(a, c);
}

TEST(RingBufferInbound, GetWrapsAroundMask) {
    RingBuffer_inbound rb(8);
    // slot 0 and slot 8 should point to the same underlying storage
    EXPECT_EQ(rb.get(0), rb.get(8));
    EXPECT_EQ(rb.get(3), rb.get(11));
}

TEST(RingBufferInbound, PublishAndReleaseTogglesIsReady) {
    RingBuffer_inbound rb(8);
    rb.claim();
    rb.publish(0);
    EXPECT_TRUE(rb.get(0)->is_ready.load());
    rb.release(0);
    EXPECT_FALSE(rb.get(0)->is_ready.load());
}

TEST(RingBufferInbound, DataWrittenToSlotPersists) {
    RingBuffer_inbound rb(8);
    int64_t idx = rb.claim();
    Orderevent_inbound* slot = rb.get(idx);
    slot->price = 12345;
    slot->volume = 67;
    slot->side = false;
    slot->internal_order_id = 99;
    slot->gateway_id = 2;

    Orderevent_inbound* again = rb.get(idx);
    EXPECT_EQ(again->price, 12345);
    EXPECT_EQ(again->volume, 67);
    EXPECT_FALSE(again->side);
    EXPECT_EQ(again->internal_order_id, 99);
    EXPECT_EQ(again->gateway_id, 2);
}

// --- outbound ring buffer ---

TEST(RingBufferOutbound, GetWrapsAroundMask) {
    RingBuffer_outbound rb(8);
    EXPECT_EQ(rb.get(0), rb.get(8));
    EXPECT_EQ(rb.get(5), rb.get(13));
}

TEST(RingBufferOutbound, PublishAndReleaseTogglesIsReady) {
    RingBuffer_outbound rb(8);
    rb.publish(0);
    EXPECT_TRUE(rb.get(0)->is_ready.load());
    rb.release(0);
    EXPECT_FALSE(rb.get(0)->is_ready.load());
}

TEST(RingBufferOutbound, DataWrittenToSlotPersists) {
    RingBuffer_outbound rb(8);
    Orderevent_outbound* slot = rb.get(3);
    slot->order_id = 77;
    slot->gateway_id = 1;
    slot->fulfilled = true;
    slot->remaining_qty = 5;
    slot->fill_count = 2;
    slot->fills[0] = {100, 3, 1111};
    slot->fills[1] = {101, 2, 2222};

    Orderevent_outbound* again = rb.get(3);
    EXPECT_EQ(again->order_id, 77);
    EXPECT_EQ(again->gateway_id, 1);
    EXPECT_TRUE(again->fulfilled);
    EXPECT_EQ(again->remaining_qty, 5);
    EXPECT_EQ(again->fill_count, 2);
    EXPECT_EQ(again->fills[0].price, 100);
    EXPECT_EQ(again->fills[1].price, 101);
}
