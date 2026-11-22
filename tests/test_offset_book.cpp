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

TEST(OffsetArrayBook, SlidesWindowInBothDirectionsAndRetainsOverlappingLevels) {
  tickbook::OffsetArrayBook book(5);
  book.apply(update(tickbook::Side::Bid, 100, 1));
  book.apply(update(tickbook::Side::Bid, 102, 2));
  book.apply(update(tickbook::Side::Ask, 103, 3));
  book.apply(update(tickbook::Side::Ask, 104, 4));

  book.apply(update(tickbook::Side::Bid, 106, 5)); // Slides the window up to [102, 106].
  ASSERT_TRUE(book.window_base().has_value());
  EXPECT_EQ(*book.window_base(), 102);
  ASSERT_TRUE(book.best_bid().has_value());
  EXPECT_EQ(book.best_bid()->price, 106);
  EXPECT_EQ(book.bid_levels(), 2u); // 100 falls outside; 102 remains.
  EXPECT_EQ(book.ask_levels(), 2u);

  book.apply(update(tickbook::Side::Ask, 99, 6)); // Slides down to [99, 103].
  ASSERT_TRUE(book.window_base().has_value());
  EXPECT_EQ(*book.window_base(), 99);
  ASSERT_TRUE(book.best_ask().has_value());
  EXPECT_EQ(book.best_ask()->price, 99);
  EXPECT_EQ(book.bid_levels(), 1u); // 106 falls outside; 102 remains.
  EXPECT_EQ(book.ask_levels(), 2u); // 104 falls outside; 103 remains.
}

TEST(OffsetArrayBook, LargeRecenterResetsOldWindowAndHandlesPriceExtremes) {
  tickbook::OffsetArrayBook book(4);
  const auto low = std::numeric_limits<tickbook::Price>::min() + 1;
  const auto high = std::numeric_limits<tickbook::Price>::max() - 1;
  book.apply(update(tickbook::Side::Bid, low, 1));
  book.apply(update(tickbook::Side::Ask, high, 2));
  EXPECT_EQ(book.bid_levels(), 0u);
  ASSERT_TRUE(book.best_ask().has_value());
  EXPECT_EQ(book.best_ask()->price, high);
  EXPECT_EQ(book.ask_levels(), 1u);

  book.apply(update(tickbook::Side::Bid, high, 3));
  ASSERT_TRUE(book.best_bid().has_value());
  EXPECT_EQ(book.best_bid()->price, high);
  EXPECT_TRUE(book.crossed());
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
  tickbook::OffsetArrayBook book(4, 10);
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
