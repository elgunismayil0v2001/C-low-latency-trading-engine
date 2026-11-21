#include <gtest/gtest.h>

#include "tickbook/book_map.hpp"

using namespace tickbook;

static Event upd(Side s, Price p, Qty q) {
  return Event{0, 0, 0, p, q, EventType::BookUpdate, s};
}

TEST(BookMap, EmptyBookHasNoBest) {
  BookMap b;
  EXPECT_FALSE(b.best_bid().has_value());
  EXPECT_FALSE(b.best_ask().has_value());
  EXPECT_FALSE(b.crossed());
}

TEST(BookMap, TracksBestBidAndAsk) {
  BookMap b;
  b.apply(upd(Side::Bid, 100, 5));
  b.apply(upd(Side::Bid, 101, 3));
  b.apply(upd(Side::Ask, 103, 4));
  b.apply(upd(Side::Ask, 102, 2));
  EXPECT_EQ(b.best_bid()->price, 101);
  EXPECT_EQ(b.best_bid()->qty, 3);
  EXPECT_EQ(b.best_ask()->price, 102);
  EXPECT_EQ(b.best_ask()->qty, 2);
  EXPECT_FALSE(b.crossed());
}

TEST(BookMap, ZeroQuantityRemovesLevel) {
  BookMap b;
  b.apply(upd(Side::Bid, 101, 3));
  b.apply(upd(Side::Bid, 100, 5));
  b.apply(upd(Side::Bid, 101, 0));
  EXPECT_EQ(b.best_bid()->price, 100);
  EXPECT_EQ(b.bid_levels(), 1u);
}

TEST(BookMap, UpdateReplacesQuantity) {
  BookMap b;
  b.apply(upd(Side::Ask, 102, 2));
  b.apply(upd(Side::Ask, 102, 9));
  EXPECT_EQ(b.best_ask()->qty, 9);
  EXPECT_EQ(b.ask_levels(), 1u);
}

TEST(BookMap, IgnoresTrades) {
  BookMap b;
  Event t{0, 0, 0, 100, 7, EventType::Trade, Side::Bid};
  b.apply(t);
  EXPECT_EQ(b.bid_levels(), 0u);
}

TEST(BookMap, DetectsCrossedBook) {
  BookMap b;
  b.apply(upd(Side::Bid, 105, 1));
  b.apply(upd(Side::Ask, 104, 1));
  EXPECT_TRUE(b.crossed());
}

TEST(BookMap, ClearEmptiesBothSides) {
  BookMap b;
  b.apply(upd(Side::Bid, 100, 1));
  b.apply(upd(Side::Ask, 101, 1));
  b.clear();
  EXPECT_EQ(b.bid_levels() + b.ask_levels(), 0u);
}
