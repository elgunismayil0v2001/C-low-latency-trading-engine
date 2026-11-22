#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "tickbook/book_map.hpp"
#include "tickbook/books/flat_book.hpp"
#include "tickbook/books/offset_book.hpp"
#include "tickbook/books/order_book.hpp"

namespace {

using namespace tickbook;

static_assert(OrderBook<BookMap>);
static_assert(OrderBook<FlatBook>);
static_assert(OrderBook<OffsetArrayBook>);

struct RandomStream {
  std::uint64_t state{0x6a09e667f3bcc909ULL};

  [[nodiscard]] std::uint64_t next() noexcept {
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
  }
};

template <OrderBook Left, OrderBook Right> bool same_book(const Left& left, const Right& right) {
  if (left.best_bid() != right.best_bid() || left.best_ask() != right.best_ask() ||
      left.bid_levels() != right.bid_levels() || left.ask_levels() != right.ask_levels() ||
      left.crossed() != right.crossed()) {
    return false;
  }

  std::vector<Level> left_bids;
  std::vector<Level> right_bids;
  std::vector<Level> left_asks;
  std::vector<Level> right_asks;
  left.visit_bids([&left_bids](Price price, Qty qty) { left_bids.push_back({price, qty}); });
  right.visit_bids([&right_bids](Price price, Qty qty) { right_bids.push_back({price, qty}); });
  left.visit_asks([&left_asks](Price price, Qty qty) { left_asks.push_back({price, qty}); });
  right.visit_asks([&right_asks](Price price, Qty qty) { right_asks.push_back({price, qty}); });

  if (left_bids.size() != right_bids.size() || left_asks.size() != right_asks.size()) return false;
  for (std::size_t i = 0; i < left_bids.size(); ++i) {
    if (left_bids[i].price != right_bids[i].price || left_bids[i].qty != right_bids[i].qty) {
      return false;
    }
  }
  for (std::size_t i = 0; i < left_asks.size(); ++i) {
    if (left_asks[i].price != right_asks[i].price || left_asks[i].qty != right_asks[i].qty) {
      return false;
    }
  }
  return true;
}

Event next_event(RandomStream& random) {
  Event event{};
  const auto value = random.next();
  const auto kind = value % 16;
  event.type = kind == 0 ? EventType::Trade : EventType::BookUpdate;
  event.side = ((value >> 8U) & 1U) == 0 ? Side::Bid : Side::Ask;
  event.price = 10'000 + static_cast<Price>((value >> 9U) % 401) - 200;
  const auto quantity = (value >> 24U) % 10;
  event.qty = quantity == 0 ? 0 : static_cast<Qty>(quantity);
  if (((value >> 32U) & 0x3ffU) == 0) event.qty = -1;
  return event;
}

} // namespace

TEST(OrderBooks, RandomUpdatesMatchBookMapAfterEveryEvent) {
  BookMap baseline;
  FlatBook flat(512);
  OffsetArrayBook offset(512);
  RandomStream random;

  constexpr std::size_t kEventCount = 10'000;
  for (std::size_t i = 0; i < kEventCount; ++i) {
    const auto event = next_event(random);
    baseline.apply(event);
    flat.apply(event);
    offset.apply(event);

    SCOPED_TRACE(i);
    EXPECT_TRUE(same_book(baseline, flat));
    EXPECT_TRUE(same_book(baseline, offset));
  }
}

TEST(OrderBooks, BothOptimizedBooksMatchAfterClearAndSnapshotSizedUpdates) {
  BookMap baseline;
  FlatBook flat(32);
  OffsetArrayBook offset(32);
  const std::array events{
      Event{0, 0, 0, 100, 3, EventType::SnapshotLevel, Side::Bid},
      Event{0, 0, 0, 101, 4, EventType::SnapshotLevel, Side::Ask},
      Event{0, 0, 0, 99, 7, EventType::SnapshotLevel, Side::Bid},
      Event{0, 0, 0, 102, 8, EventType::SnapshotLevel, Side::Ask},
      Event{1, 0, 0, 100, 0, EventType::BookUpdate, Side::Bid},
      Event{1, 0, 0, 101, 5, EventType::BookUpdate, Side::Ask},
  };

  for (const auto& event : events) {
    baseline.apply(event);
    flat.apply(event);
    offset.apply(event);
  }
  EXPECT_TRUE(same_book(baseline, flat));
  EXPECT_TRUE(same_book(baseline, offset));

  baseline.clear();
  flat.clear();
  offset.clear();
  EXPECT_TRUE(same_book(baseline, flat));
  EXPECT_TRUE(same_book(baseline, offset));
}

TEST(OrderBooks, VisitorsCanStopAfterTheRequestedTopLevels) {
  BookMap baseline;
  FlatBook flat(16);
  OffsetArrayBook offset(64);
  for (Price price = 100; price < 108; ++price) {
    const Event bid{0, 0, 0, price, 1, EventType::BookUpdate, Side::Bid};
    const Event ask{0, 0, 0, price + 20, 1, EventType::BookUpdate, Side::Ask};
    baseline.apply(bid);
    baseline.apply(ask);
    flat.apply(bid);
    flat.apply(ask);
    offset.apply(bid);
    offset.apply(ask);
  }

  const auto count_one_bid = [](const auto& book) {
    std::size_t visited = 0;
    book.visit_bids([&visited](Price, Qty) noexcept {
      ++visited;
      return false;
    });
    return visited;
  };
  const auto count_one_ask = [](const auto& book) {
    std::size_t visited = 0;
    book.visit_asks([&visited](Price, Qty) noexcept {
      ++visited;
      return false;
    });
    return visited;
  };

  EXPECT_EQ(count_one_bid(baseline), 1U);
  EXPECT_EQ(count_one_bid(flat), 1U);
  EXPECT_EQ(count_one_bid(offset), 1U);
  EXPECT_EQ(count_one_ask(baseline), 1U);
  EXPECT_EQ(count_one_ask(flat), 1U);
  EXPECT_EQ(count_one_ask(offset), 1U);
}
