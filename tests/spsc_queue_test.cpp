#include <gtest/gtest.h>
#include "spsc_queue.hpp"
#include <thread>
#include <atomic>

TEST(SpscQueueTest, PopFromEmptyReturnsFalse) {
    SpscQueue<int> q(8);
    int out = 42;
    EXPECT_FALSE(q.try_pop(out));
    EXPECT_EQ(out, 42); // out unchanged on empty
}

TEST(SpscQueueTest, PushThenPopReturnsSameValue) {
    SpscQueue<int> q(8);
    EXPECT_TRUE(q.try_push(7));
    int out = 0;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 7);
}

TEST(SpscQueueTest, FifoOrderPreserved) {
    SpscQueue<int> q(8);
    for (int i = 0; i < 5; i++) ASSERT_TRUE(q.try_push(i));
    for (int i = 0; i < 5; i++) {
        int out = -1;
        ASSERT_TRUE(q.try_pop(out));
        EXPECT_EQ(out, i);
    }
}

TEST(SpscQueueTest, FullQueueRejectsPush) {
    SpscQueue<int> q(4);
    for (int i = 0; i < 4; i++) ASSERT_TRUE(q.try_push(i));
    EXPECT_FALSE(q.try_push(99));    // full
    int out = -1;
    ASSERT_TRUE(q.try_pop(out));     // drain one slot
    EXPECT_EQ(out, 0);
    EXPECT_TRUE(q.try_push(99));     // now there is room
}

TEST(SpscQueueTest, WrapsAroundCorrectly) {
    SpscQueue<int> q(4);
    // Fill, drain, fill again — exercises the mask wrap.
    for (int i = 0; i < 4; i++) ASSERT_TRUE(q.try_push(i));
    for (int i = 0; i < 4; i++) { int out; ASSERT_TRUE(q.try_pop(out)); }
    for (int i = 100; i < 104; i++) ASSERT_TRUE(q.try_push(i));
    for (int i = 100; i < 104; i++) {
        int out = -1;
        ASSERT_TRUE(q.try_pop(out));
        EXPECT_EQ(out, i);
    }
}

TEST(SpscQueueTest, ProducerConsumerThreadsTransferAllItems) {
    // One producer thread, one consumer thread — verify nothing is lost or reordered.
    constexpr int64_t N = 100000;
    SpscQueue<int64_t> q(1024);

    std::thread producer([&]{
        for (int64_t i = 0; i < N; i++) {
            while (!q.try_push(i)) {} // spin until there's room
        }
    });

    int64_t expected = 0;
    while (expected < N) {
        int64_t out;
        if (q.try_pop(out)) {
            ASSERT_EQ(out, expected);
            expected++;
        }
    }
    producer.join();
    EXPECT_EQ(expected, N);
}
