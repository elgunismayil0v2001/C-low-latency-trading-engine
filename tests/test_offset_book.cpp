#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "tickbook/books/offset_book.hpp"

namespace {

tickbook::Event update(tickbook::Side side, tickbook::Price price, tickbook::Qty qty,
                       tickbook::EventType type = tickbook::EventType::BookUpdate) {
  tickbook::Event event{};
  event.type = type;
  event.side = side;
  event.price = price;
  event.qty = qty;
  return event;
}

} // namespace

TEST(OffsetArrayBook, TracksBestLevelsAndRemovesTheBestWithoutScanningOnEveryUpdate) {
  tickbook::OffsetArrayBook book(16);
  book.apply(update(tickbook::Side::Bid, 100, 4));
  book.apply(update(tickbook::Side::Bid, 102, 6));
  book.apply(update(tickbook::Side::Ask, 105, 8));
  book.apply(update(tickbook::Side::Ask, 103, 7));

  ASSERT_TRUE(book.best_bid().has_value());
  EXPECT_EQ(book.best_bid()->price, 102);
  ASSERT_TRUE(book.best_ask().has_value());
  EXPECT_EQ(book.best_ask()->price, 103);
  EXPECT_EQ(book.bid_levels(), 2u);
  EXPECT_EQ(book.ask_levels(), 2u);

  book.apply(update(tickbook::Side::Bid, 102, 0));
  book.apply(update(tickbook::Side::Ask, 103, 0));
  ASSERT_TRUE(book.best_bid().has_value());
  EXPECT_EQ(book.best_bid()->price, 100);
  ASSERT_TRUE(book.best_ask().has_value());
  EXPECT_EQ(book.best_ask()->price, 105);

  std::vector<tickbook::Price> bids;
  std::vector<tickbook::Price> asks;
  book.visit_bids([&bids](tickbook::Price price, tickbook::Qty) { bids.push_back(price); });
  book.visit_asks([&asks](tickbook::Price price, tickbook::Qty) { asks.push_back(price); });
  EXPECT_EQ(bids, (std::vector<tickbook::Price>{100}));
  EXPECT_EQ(asks, (std::vector<tickbook::Price>{105}));
}

TEST(OffsetArrayBook, IgnoresAFarUpdateWhenTheBestLevelsAreAwayFromTheEdges) {
  tickbook::OffsetArrayBook book(16);
  book.apply(update(tickbook::Side::Bid, 100, 1));
  book.apply(update(tickbook::Side::Ask, 101, 2));
  const auto base = book.window_base();

  book.apply(update(tickbook::Side::Ask, 1'000, 5));
  book.apply(update(tickbook::Side::Bid, -1'000, 6));
  ASSERT_TRUE(book.window_base().has_value());
  EXPECT_EQ(book.window_base(), base);
  ASSERT_TRUE(book.best_bid().has_value());
  EXPECT_EQ(book.best_bid()->price, 100);
  EXPECT_EQ(book.bid_levels(), 1u);
  ASSERT_TRUE(book.best_ask().has_value());
  EXPECT_EQ(book.best_ask()->price, 101);
  EXPECT_EQ(book.ask_levels(), 1u);
}

TEST(OffsetArrayBook, OutOfWindowUpdatesAreIgnoredAtPriceExtremes) {
  tickbook::OffsetArrayBook book(4);
  const auto low = std::numeric_limits<tickbook::Price>::min() + 1;
  const auto high = std::numeric_limits<tickbook::Price>::max() - 1;
  book.apply(update(tickbook::Side::Bid, low, 1));
  book.apply(update(tickbook::Side::Ask, high, 2));
  ASSERT_TRUE(book.best_bid().has_value());
  EXPECT_EQ(book.best_bid()->price, low);
  EXPECT_EQ(book.bid_levels(), 1u);
  EXPECT_FALSE(book.best_ask().has_value());
  EXPECT_EQ(book.ask_levels(), 0u);

  book.apply(update(tickbook::Side::Bid, high, 3));
  EXPECT_EQ(book.best_bid()->price, low);
  EXPECT_FALSE(book.crossed());
}

TEST(OffsetArrayBook, IgnoresInvalidEventsAndClearResetsTheWindow) {
  tickbook::OffsetArrayBook book(8);
  book.apply(update(tickbook::Side::Bid, 100, -1));
  book.apply(update(tickbook::Side::Ask, 101, 2, tickbook::EventType::Trade));
  EXPECT_FALSE(book.window_base().has_value());
  EXPECT_EQ(book.bid_levels() + book.ask_levels(), 0u);

  book.apply(update(tickbook::Side::Bid, 100, 1));
  book.clear();
  EXPECT_FALSE(book.window_base().has_value());
  EXPECT_FALSE(book.best_bid().has_value());
  EXPECT_FALSE(book.best_ask().has_value());
}

TEST(OffsetArrayBook, EmptyWindowIgnoresUpdates) {
  tickbook::OffsetArrayBook book(0);
  book.apply(update(tickbook::Side::Bid, 100, 1));
  EXPECT_FALSE(book.best_bid().has_value());
  EXPECT_EQ(book.bid_levels(), 0u);
}

TEST(OffsetArrayBook, IndexesConfiguredPriceTicksAndRejectsMisalignedPrices) {
  tickbook::OffsetArrayBook book(6, 10);
  book.apply(update(tickbook::Side::Bid, 100, 1));
  book.apply(update(tickbook::Side::Bid, 110, 2));
  book.apply(update(tickbook::Side::Bid, 105, 3));
  book.apply(update(tickbook::Side::Ask, 120, 4));

  ASSERT_TRUE(book.best_bid().has_value());
  EXPECT_EQ(book.best_bid()->price, 110);
  EXPECT_EQ(book.bid_levels(), 2u);
  ASSERT_TRUE(book.best_ask().has_value());
  EXPECT_EQ(book.best_ask()->price, 120);
  EXPECT_EQ(book.ask_levels(), 1u);
}

TEST(OffsetArrayBook, DefaultWindowIgnoresAFarPriceWithoutClearingBook) {
  tickbook::OffsetArrayBook book;
  book.apply(update(tickbook::Side::Bid, 10'000, 3));
  book.apply(update(tickbook::Side::Ask, 10'001, 4));
  const auto previous_bid = book.best_bid();
  const auto previous_ask = book.best_ask();
  const auto previous_bid_levels = book.bid_levels();
  const auto previous_ask_levels = book.ask_levels();

  book.apply(update(tickbook::Side::Ask, 100'000, 9));

  EXPECT_EQ(book.best_bid(), previous_bid);
  EXPECT_EQ(book.best_ask(), previous_ask);
  EXPECT_EQ(book.bid_levels(), previous_bid_levels);
  EXPECT_EQ(book.ask_levels(), previous_ask_levels);
}

TEST(OffsetArrayBook, RecenteringKeepsEveryLevelInsideTheShiftedWindow) {
  tickbook::OffsetArrayBook book(16);
  book.apply(update(tickbook::Side::Bid, 100, 1));
  book.apply(update(tickbook::Side::Bid, 97, 3));
  book.apply(update(tickbook::Side::Bid, 101, 5));
  book.apply(update(tickbook::Side::Ask, 102, 4));

  book.apply(update(tickbook::Side::Bid, 104, 6));

  EXPECT_EQ(book.window_base(), std::optional<tickbook::Price>{100});
  EXPECT_EQ(book.bid_levels(), 3u);
  EXPECT_EQ(book.ask_levels(), 1u);
  std::vector<tickbook::Level> bids;
  std::vector<tickbook::Level> asks;
  book.visit_bids(
      [&bids](tickbook::Price price, tickbook::Qty qty) { bids.push_back({price, qty}); });
  book.visit_asks(
      [&asks](tickbook::Price price, tickbook::Qty qty) { asks.push_back({price, qty}); });
  EXPECT_EQ(bids, (std::vector<tickbook::Level>{{104, 6}, {101, 5}, {100, 1}}));
  EXPECT_EQ(asks, (std::vector<tickbook::Level>{{102, 4}}));
}
