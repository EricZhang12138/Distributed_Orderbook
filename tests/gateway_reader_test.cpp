#include <gtest/gtest.h>
#include "gateway.hpp"
#include "gateway_reader.hpp"
#include "ring_buffer.hpp"
#include "spsc_queue.hpp"
#include <chrono>
#include <thread>

// These tests exercise GatewayReader against a manually-populated outbound
// ring buffer (no real matching engine thread). They verify that the reader
// drains the outbound, repackages each ack into an Ack, and pushes it into
// the queue — and that overflow correctly increments the drop counter.

namespace {
// Helper: poll for `target` items in the queue, up to a short timeout. Returns
// the number of items actually popped, in order, into `out`.
int64_t drain_with_timeout(SpscQueue<Ack>& q, std::vector<Ack>& out,
                           int64_t target, int ms_timeout = 500) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms_timeout);
    while ((int64_t)out.size() < target && std::chrono::steady_clock::now() < deadline) {
        Ack a;
        if (q.try_pop(a)) out.push_back(a);
    }
    return out.size();
}

// Helper: write one ack into the engine's view of the outbound buffer.
void inject_ack(RingBuffer_outbound& outbound, int64_t engine_write_p,
                Op op, int64_t order_id, bool fulfilled, int64_t remaining_qty) {
    Orderevent_outbound* slot = outbound.get(engine_write_p);
    slot->op = op;
    slot->order_id = order_id;
    slot->fulfilled = fulfilled;
    slot->remaining_qty = remaining_qty;
    slot->fill_count = 0;
    outbound.publish(engine_write_p);
}
} // namespace

TEST(GatewayReaderTest, ReaderDrainsSingleAckIntoQueue) {
    RingBuffer_inbound inbound(8);
    RingBuffer_outbound outbound(8);
    Gateway gw(&inbound, &outbound, 0);
    SpscQueue<Ack> queue(8);

    {
        GatewayReader reader(&gw, &queue);
        inject_ack(outbound, 0, Op::NEW, 42, true, 0);

        std::vector<Ack> out;
        ASSERT_EQ(drain_with_timeout(queue, out, 1), 1);
        EXPECT_EQ(out[0].op, Op::NEW);
        EXPECT_EQ(out[0].order_id, 42);
        EXPECT_TRUE(out[0].fulfilled);
    } // reader destructor joins the thread cleanly
}

TEST(GatewayReaderTest, ReaderPreservesAckOrderAndOpFields) {
    RingBuffer_inbound inbound(16);
    RingBuffer_outbound outbound(16);
    Gateway gw(&inbound, &outbound, 0);
    SpscQueue<Ack> queue(16);

    {
        GatewayReader reader(&gw, &queue);
        inject_ack(outbound, 0, Op::NEW,    100, false, 5);
        inject_ack(outbound, 1, Op::CANCEL, 100, true,  0);
        inject_ack(outbound, 2, Op::MODIFY, 200, true,  0);

        std::vector<Ack> out;
        ASSERT_EQ(drain_with_timeout(queue, out, 3), 3);
        EXPECT_EQ(out[0].op, Op::NEW);    EXPECT_EQ(out[0].order_id, 100); EXPECT_EQ(out[0].remaining_qty, 5);
        EXPECT_EQ(out[1].op, Op::CANCEL); EXPECT_EQ(out[1].order_id, 100); EXPECT_TRUE(out[1].fulfilled);
        EXPECT_EQ(out[2].op, Op::MODIFY); EXPECT_EQ(out[2].order_id, 200); EXPECT_TRUE(out[2].fulfilled);
    }
}

TEST(GatewayReaderTest, ReaderCopiesFillsArray) {
    RingBuffer_inbound inbound(8);
    RingBuffer_outbound outbound(8);
    Gateway gw(&inbound, &outbound, 0);
    SpscQueue<Ack> queue(8);

    {
        GatewayReader reader(&gw, &queue);
        Orderevent_outbound* slot = outbound.get(0);
        slot->op = Op::NEW;
        slot->order_id = 7;
        slot->fulfilled = true;
        slot->fill_count = 2;
        slot->fills[0] = {100, 4, 111};
        slot->fills[1] = {101, 6, 222};
        outbound.publish(0);

        std::vector<Ack> out;
        ASSERT_EQ(drain_with_timeout(queue, out, 1), 1);
        ASSERT_EQ(out[0].fill_count, 2);
        EXPECT_EQ(out[0].fills[0].price, 100);
        EXPECT_EQ(out[0].fills[0].volume, 4);
        EXPECT_EQ(out[0].fills[1].price, 101);
        EXPECT_EQ(out[0].fills[1].volume, 6);
    }
}

TEST(GatewayReaderTest, OverflowIncrementsDropCounter) {
    // Queue is tiny (capacity 2). Inject more than 2 acks before any pop.
    // Reader must drop the surplus and bump dropped_count, never block.
    RingBuffer_inbound inbound(16);
    RingBuffer_outbound outbound(16);
    Gateway gw(&inbound, &outbound, 0);
    SpscQueue<Ack> queue(2);  // intentionally too small

    GatewayReader reader(&gw, &queue);
    for (int i = 0; i < 6; i++) {
        inject_ack(outbound, i, Op::NEW, i, true, 0);
    }

    // Give the reader some time to attempt all 6 pushes.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        if (reader.dropped_count() >= 4) break;  // 6 attempts - 2 queue slots = 4 drops
        std::this_thread::yield();
    }

    EXPECT_GE(reader.dropped_count(), 4);
}

TEST(GatewayReaderTest, DestructorJoinsCleanlyOnEmptyQueue) {
    // Smoke test: create and destroy a reader without any traffic.
    // If the destructor failed to join, this would hang or std::terminate.
    RingBuffer_inbound inbound(8);
    RingBuffer_outbound outbound(8);
    Gateway gw(&inbound, &outbound, 0);
    SpscQueue<Ack> queue(8);

    GatewayReader reader(&gw, &queue);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // reader goes out of scope at end of test — destructor must return.
    SUCCEED();
}
