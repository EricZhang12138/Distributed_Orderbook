#include <gtest/gtest.h>
#include "orderbook.hpp"

// --- single-side resting (no crosses) ---

TEST(OrderbookTest, SellRestsWhenNoBids) {
    Orderbook ob;
    auto [res, gid] = ob.placeOrder(100, 10, true, 1, 0, 0);
    EXPECT_FALSE(res.fulfilled);
    EXPECT_EQ(res.remainingQty, 10);
    EXPECT_EQ(res.trades.size(), 0);
    EXPECT_EQ(gid, 0);
}

TEST(OrderbookTest, BuyRestsWhenNoAsks) {
    Orderbook ob;
    auto [res, gid] = ob.placeOrder(100, 10, false, 1, 0, 0);
    EXPECT_FALSE(res.fulfilled);
    EXPECT_EQ(res.remainingQty, 10);
    EXPECT_EQ(res.trades.size(), 0);
}

// --- simple full matches ---

TEST(OrderbookTest, BuyExactlyFillsSingleSell) {
    Orderbook ob;
    ob.placeOrder(100, 10, true, 1, 0, 0);           // sell 10 @ 100
    auto [res, gid] = ob.placeOrder(100, 10, false, 2, 1, 0); // buy  10 @ 100
    EXPECT_TRUE(res.fulfilled);
    EXPECT_EQ(res.remainingQty, 0);
    ASSERT_EQ(res.trades.size(), 1);
    EXPECT_EQ(res.trades[0].price, 100);
    EXPECT_EQ(res.trades[0].volume, 10);
}

TEST(OrderbookTest, SellExactlyFillsSingleBuy) {
    Orderbook ob;
    ob.placeOrder(100, 10, false, 1, 0, 0);          // buy  10 @ 100
    auto [res, gid] = ob.placeOrder(100, 10, true, 2, 1, 0);  // sell 10 @ 100
    EXPECT_TRUE(res.fulfilled);
    EXPECT_EQ(res.remainingQty, 0);
    ASSERT_EQ(res.trades.size(), 1);
    EXPECT_EQ(res.trades[0].volume, 10);
}

// --- partial fills ---

TEST(OrderbookTest, BuyPartiallyFillsSell_RestRemains) {
    Orderbook ob;
    ob.placeOrder(100, 10, true, 1, 0, 0);            // sell 10 @ 100
    auto [res, gid] = ob.placeOrder(100, 3, false, 2, 1, 0);   // buy  3 @ 100
    EXPECT_TRUE(res.fulfilled);
    EXPECT_EQ(res.remainingQty, 0);
    ASSERT_EQ(res.trades.size(), 1);
    EXPECT_EQ(res.trades[0].volume, 3);

    // the remaining 7 should still be on the book, matched by a second buy
    auto [res2, gid2] = ob.placeOrder(100, 7, false, 3, 2, 0);
    EXPECT_TRUE(res2.fulfilled);
    EXPECT_EQ(res2.remainingQty, 0);
    ASSERT_EQ(res2.trades.size(), 1);
    EXPECT_EQ(res2.trades[0].volume, 7);
}

TEST(OrderbookTest, BuyLargerThanSell_LeftoverRestsInBook) {
    Orderbook ob;
    ob.placeOrder(100, 5, true, 1, 0, 0);             // sell 5 @ 100
    auto [res, gid] = ob.placeOrder(100, 12, false, 2, 1, 0);  // buy  12 @ 100
    EXPECT_FALSE(res.fulfilled); // leftover 7 should rest
    EXPECT_EQ(res.remainingQty, 7);
    ASSERT_EQ(res.trades.size(), 1);
    EXPECT_EQ(res.trades[0].volume, 5);

    // a later sell of 7 @ 100 should fully consume the resting bid
    auto [res2, gid2] = ob.placeOrder(100, 7, true, 3, 2, 0);
    EXPECT_TRUE(res2.fulfilled);
    EXPECT_EQ(res2.remainingQty, 0);
}

// --- price priority (multiple levels) ---

TEST(OrderbookTest, BuySweepsMultipleAskLevels_LowestFirst) {
    Orderbook ob;
    ob.placeOrder(100, 5, true, 1, 0, 0);
    ob.placeOrder(101, 5, true, 2, 1, 0);
    ob.placeOrder(102, 5, true, 3, 2, 0);

    auto [res, gid] = ob.placeOrder(102, 12, false, 4, 3, 0); // buy 12 @ 102
    EXPECT_TRUE(res.fulfilled);
    EXPECT_EQ(res.remainingQty, 0);
    ASSERT_EQ(res.trades.size(), 3);
    EXPECT_EQ(res.trades[0].price, 100);
    EXPECT_EQ(res.trades[1].price, 101);
    EXPECT_EQ(res.trades[2].price, 102);
    EXPECT_EQ(res.trades[0].volume, 5);
    EXPECT_EQ(res.trades[1].volume, 5);
    EXPECT_EQ(res.trades[2].volume, 2);
}

TEST(OrderbookTest, SellSweepsMultipleBidLevels_HighestFirst) {
    Orderbook ob;
    ob.placeOrder(100, 5, false, 1, 0, 0);
    ob.placeOrder(101, 5, false, 2, 1, 0);
    ob.placeOrder(102, 5, false, 3, 2, 0);

    auto [res, gid] = ob.placeOrder(100, 12, true, 4, 3, 0); // sell 12 @ 100
    EXPECT_TRUE(res.fulfilled);
    EXPECT_EQ(res.remainingQty, 0);
    ASSERT_EQ(res.trades.size(), 3);
    EXPECT_EQ(res.trades[0].price, 102);
    EXPECT_EQ(res.trades[1].price, 101);
    EXPECT_EQ(res.trades[2].price, 100);
}

// --- time priority within a single price level ---

TEST(OrderbookTest, FifoWithinPriceLevel) {
    Orderbook ob;
    ob.placeOrder(100, 4, true, 1, 0, 0); // first sell
    ob.placeOrder(100, 6, true, 2, 1, 0); // second sell at same price

    // first buy takes 3 — should eat into the first sell only
    auto [res1, g1] = ob.placeOrder(100, 3, false, 3, 2, 0);
    EXPECT_TRUE(res1.fulfilled);
    ASSERT_EQ(res1.trades.size(), 1);
    EXPECT_EQ(res1.trades[0].volume, 3);

    // second buy takes 5 — should finish the first sell (1 left) and take 4 from the second
    auto [res2, g2] = ob.placeOrder(100, 5, false, 4, 3, 0);
    EXPECT_TRUE(res2.fulfilled);
    ASSERT_EQ(res2.trades.size(), 2);
    EXPECT_EQ(res2.trades[0].volume, 1); // leftover from first sell
    EXPECT_EQ(res2.trades[1].volume, 4); // into second sell
}

// --- non-crossing limits ---

TEST(OrderbookTest, NonCrossingBuyRests) {
    Orderbook ob;
    ob.placeOrder(105, 10, true, 1, 0, 0);            // sell @ 105
    auto [res, gid] = ob.placeOrder(100, 10, false, 2, 1, 0); // buy @ 100 (doesn't cross)
    EXPECT_FALSE(res.fulfilled);
    EXPECT_EQ(res.remainingQty, 10);
    EXPECT_EQ(res.trades.size(), 0);
}

// --- cancel ---

TEST(OrderbookTest, CancelExistingOrder) {
    Orderbook ob;
    ob.placeOrder(100, 10, true, 1, 42, 0);
    EXPECT_TRUE(ob.cancel(42));
    // a buy at the same price should now not match
    auto [res, gid] = ob.placeOrder(100, 10, false, 2, 43, 0);
    EXPECT_FALSE(res.fulfilled);
    EXPECT_EQ(res.trades.size(), 0);
}

TEST(OrderbookTest, CancelNonexistentOrderReturnsFalse) {
    Orderbook ob;
    EXPECT_FALSE(ob.cancel(9999));
}

TEST(OrderbookTest, CancelHeadOfThreeOrdersSamePrice) {
    Orderbook ob;
    ob.placeOrder(100, 1, true, 1, 10, 0);
    ob.placeOrder(100, 2, true, 2, 11, 0);
    ob.placeOrder(100, 3, true, 3, 12, 0);
    EXPECT_TRUE(ob.cancel(10));
    // a buy of 5 should match the remaining two (2 + 3)
    auto [res, gid] = ob.placeOrder(100, 5, false, 4, 13, 0);
    EXPECT_TRUE(res.fulfilled);
    ASSERT_EQ(res.trades.size(), 2);
    EXPECT_EQ(res.trades[0].volume, 2);
    EXPECT_EQ(res.trades[1].volume, 3);
}

TEST(OrderbookTest, CancelMiddleOfThreeOrdersSamePrice) {
    Orderbook ob;
    ob.placeOrder(100, 1, true, 1, 10, 0);
    ob.placeOrder(100, 2, true, 2, 11, 0);
    ob.placeOrder(100, 3, true, 3, 12, 0);
    EXPECT_TRUE(ob.cancel(11));
    auto [res, gid] = ob.placeOrder(100, 4, false, 4, 13, 0);
    EXPECT_TRUE(res.fulfilled);
    ASSERT_EQ(res.trades.size(), 2);
    EXPECT_EQ(res.trades[0].volume, 1);
    EXPECT_EQ(res.trades[1].volume, 3);
}

TEST(OrderbookTest, CancelTailOfThreeOrdersSamePrice) {
    Orderbook ob;
    ob.placeOrder(100, 1, true, 1, 10, 0);
    ob.placeOrder(100, 2, true, 2, 11, 0);
    ob.placeOrder(100, 3, true, 3, 12, 0);
    EXPECT_TRUE(ob.cancel(12));
    auto [res, gid] = ob.placeOrder(100, 3, false, 4, 13, 0);
    EXPECT_TRUE(res.fulfilled);
    ASSERT_EQ(res.trades.size(), 2);
    EXPECT_EQ(res.trades[0].volume, 1);
    EXPECT_EQ(res.trades[1].volume, 2);
}

TEST(OrderbookTest, CancelOnlyOrderPrunesLimit) {
    Orderbook ob;
    ob.placeOrder(100, 10, true, 1, 10, 0);
    EXPECT_TRUE(ob.cancel(10));
    // placing another sell at 100 should behave like a fresh level
    auto [res, gid] = ob.placeOrder(100, 5, true, 2, 11, 0);
    EXPECT_FALSE(res.fulfilled);
    EXPECT_EQ(res.remainingQty, 5);
}

TEST(OrderbookTest, CancelTwiceReturnsFalseSecondTime) {
    Orderbook ob;
    ob.placeOrder(100, 10, true, 1, 50, 0);
    EXPECT_TRUE(ob.cancel(50));
    EXPECT_FALSE(ob.cancel(50));
}
