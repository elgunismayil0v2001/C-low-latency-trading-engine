#include <gtest/gtest.h>

#include <vector>

#include "tickbook/books/flat_book.hpp"

namespace tickbook {
namespace {

Event update(Side side, Price price, Qty qty, EventType type = EventType::BookUpdate) {
  Event event{};
  event.type = type;
  event.side = side;
  event.price = price;
  event.qty = qty;
  return event;
}

} // namespace
} // namespace tickbook

TEST(FlatBook, OrdersLevelsAndUpdatesBestInConstantTime) {
  tickbook::FlatBook book(8);
  book.apply(tickbook::update(tickbook::Side::Bid, 100, 4));
  book.apply(tickbook::update(tickbook::Side::Bid, 102, 6));
  book.apply(tickbook::update(tickbook::Side::Bid, 101, 5));
  book.apply(tickbook::update(tickbook::Side::Ask, 105, 8));
  book.apply(tickbook::update(tickbook::Side::Ask, 103, 7));
  book.apply(tickbook::update(tickbook::Side::Ask, 104, 9));
  book.apply(tickbook::update(tickbook::Side::Bid, 102, 11));

  ASSERT_TRUE(book.best_bid().has_value());
  EXPECT_EQ(book.best_bid()->price, 102);
  EXPECT_EQ(book.best_bid()->qty, 11);
  ASSERT_TRUE(book.best_ask().has_value());
  EXPECT_EQ(book.best_ask()->price, 103);
  EXPECT_EQ(book.best_ask()->qty, 7);
  EXPECT_EQ(book.bid_levels(), 3u);
  EXPECT_EQ(book.ask_levels(), 3u);

  std::vector<tickbook::Level> bids;
  std::vector<tickbook::Level> asks;
  book.visit_bids(
      [&bids](tickbook::Price price, tickbook::Qty qty) { bids.push_back({price, qty}); });
  book.visit_asks(
      [&asks](tickbook::Price price, tickbook::Qty qty) { asks.push_back({price, qty}); });
  ASSERT_EQ(bids.size(), 3u);
  EXPECT_EQ(bids[0].price, 102);
  EXPECT_EQ(bids[1].price, 101);
  EXPECT_EQ(bids[2].price, 100);
  ASSERT_EQ(asks.size(), 3u);
  EXPECT_EQ(asks[0].price, 103);
  EXPECT_EQ(asks[1].price, 104);
  EXPECT_EQ(asks[2].price, 105);
}

TEST(FlatBook, RemovesLevelsAndIgnoresInvalidUpdatesAndTrades) {
  tickbook::FlatBook book(4);
  book.apply(tickbook::update(tickbook::Side::Bid, 101, 5));
  book.apply(tickbook::update(tickbook::Side::Bid, 100, 4));
  book.apply(tickbook::update(tickbook::Side::Bid, 101, 0));
  book.apply(tickbook::update(tickbook::Side::Ask, 102, -1));
  book.apply(tickbook::update(tickbook::Side::Ask, 102, 8, tickbook::EventType::Trade));

  ASSERT_TRUE(book.best_bid().has_value());
  EXPECT_EQ(book.best_bid()->price, 100);
  EXPECT_EQ(book.bid_levels(), 1u);
  EXPECT_EQ(book.ask_levels(), 0u);
  EXPECT_FALSE(book.crossed());
}

TEST(FlatBook, RetainsOnlyTheBestConfiguredDepth) {
  tickbook::FlatBook book(2);
  book.apply(tickbook::update(tickbook::Side::Bid, 100, 1));
  book.apply(tickbook::update(tickbook::Side::Bid, 99, 1));
  book.apply(tickbook::update(tickbook::Side::Bid, 98, 1));
  EXPECT_EQ(book.bid_levels(), 2u);

  book.apply(tickbook::update(tickbook::Side::Bid, 101, 1));
  ASSERT_TRUE(book.best_bid().has_value());
  EXPECT_EQ(book.best_bid()->price, 101);
  std::vector<tickbook::Price> prices;
  book.visit_bids([&prices](tickbook::Price price, tickbook::Qty) { prices.push_back(price); });
  EXPECT_EQ(prices, (std::vector<tickbook::Price>{101, 100}));

  book.apply(tickbook::update(tickbook::Side::Bid, 99, 0));
  EXPECT_EQ(book.bid_levels(), 2u);
}

TEST(FlatBook, ClearKeepsConfiguredCapacityAndEmptiesBothSides) {
  tickbook::FlatBook book(3);
  book.apply(tickbook::update(tickbook::Side::Bid, 100, 1));
  book.apply(tickbook::update(tickbook::Side::Ask, 101, 1));
  book.clear();
  EXPECT_EQ(book.capacity(), 3u);
  EXPECT_EQ(book.bid_levels() + book.ask_levels(), 0u);
  EXPECT_FALSE(book.best_bid().has_value());
  EXPECT_FALSE(book.best_ask().has_value());
}
