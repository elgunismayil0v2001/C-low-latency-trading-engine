#include <gtest/gtest.h>

#include <cstdint>

#include "tickbook/book_map.hpp"
#include "tickbook/features/feature_engine.hpp"

namespace {

tickbook::Event event(std::int64_t ts_ns, tickbook::Side side, tickbook::Price price,
                      tickbook::Qty qty, tickbook::EventType type,
                      std::uint8_t flags = tickbook::kEventFlagEndOfMessage) {
  tickbook::Event result{};
  result.ts_ns = ts_ns;
  result.price = price;
  result.qty = qty;
  result.side = side;
  result.type = type;
  result.flags = flags;
  return result;
}

tickbook::FeatureSnapshot apply(tickbook::FeatureEngine& engine, tickbook::BookMap& book,
                                const tickbook::Event& update) {
  if ((update.flags & tickbook::kEventFlagSnapshotStart) != 0) book.clear();
  book.apply(update);
  const auto expected = (update.flags & tickbook::kEventFlagEndOfMessage) != 0
                            ? tickbook::FeatureUpdateResult::Updated
                            : tickbook::FeatureUpdateResult::MessagePending;
  EXPECT_EQ(engine.on_event(update, book), expected);
  return engine.latest();
}

tickbook::FeatureSnapshot baseline(tickbook::FeatureEngine& engine, tickbook::BookMap& book) {
  apply(engine, book,
        event(0, tickbook::Side::Bid, 100, 3, tickbook::EventType::SnapshotLevel,
              tickbook::kEventFlagSnapshotStart));
  return apply(engine, book,
               event(0, tickbook::Side::Ask, 102, 1, tickbook::EventType::SnapshotLevel));
}

} // namespace

TEST(FeatureEngine, ComputesSpreadImbalanceAndMicropriceFromACompleteMessage) {
  tickbook::FeatureEngine engine({1, 8});
  tickbook::BookMap book;
  const auto snapshot = baseline(engine, book);

  EXPECT_EQ(snapshot.values.spread_ticks, 2);
  EXPECT_EQ(snapshot.values.imbalance_l1_q20, tickbook::kFeatureScale / 2);
  EXPECT_EQ(snapshot.values.imbalance_k_q20, tickbook::kFeatureScale / 2);
  EXPECT_EQ(snapshot.values.microprice_offset_q20_ticks, tickbook::kFeatureScale / 2);
  EXPECT_EQ(snapshot.values.mid_return_100ms_q20_ticks, 0);
  EXPECT_EQ(snapshot.values.mid_return_1s_q20_ticks, 0);
}

TEST(FeatureEngine, AggregatesExactlyTheTopFiveLevelsPerSide) {
  tickbook::FeatureEngine engine({1, 8});
  tickbook::BookMap book;
  for (tickbook::Price offset = 0; offset < 5; ++offset) {
    auto bid_flags = std::uint8_t{0};
    if (offset == 0) bid_flags |= tickbook::kEventFlagSnapshotStart;
    const auto bid = event(0, tickbook::Side::Bid, 100 - offset, 5,
                           tickbook::EventType::SnapshotLevel, bid_flags);
    book.apply(bid);
    EXPECT_EQ(engine.on_event(bid, book), tickbook::FeatureUpdateResult::MessagePending);
  }
  const auto ask = event(0, tickbook::Side::Ask, 102, 5, tickbook::EventType::SnapshotLevel);
  book.apply(ask);
  EXPECT_EQ(engine.on_event(ask, book), tickbook::FeatureUpdateResult::Updated);

  EXPECT_EQ(engine.latest().values.imbalance_l1_q20, 0);
  EXPECT_EQ(engine.latest().values.imbalance_k_q20, 699'050);
}

TEST(FeatureEngine, WaitsForMessageEndAndUsesBestLevelOfiWindows) {
  tickbook::FeatureEngine engine({1, 8});
  tickbook::BookMap book;
  baseline(engine, book);

  auto first = event(1, tickbook::Side::Bid, 100, 5, tickbook::EventType::BookUpdate, 0);
  book.apply(first);
  EXPECT_EQ(engine.on_event(first, book), tickbook::FeatureUpdateResult::MessagePending);
  EXPECT_EQ(engine.latest().values.ofi_10, 0);

  const auto last = event(1, tickbook::Side::Ask, 102, 1, tickbook::EventType::BookUpdate);
  book.apply(last);
  EXPECT_EQ(engine.on_event(last, book), tickbook::FeatureUpdateResult::Updated);
  EXPECT_EQ(engine.latest().values.ofi_10, 2);
  EXPECT_EQ(engine.latest().values.ofi_50, 2);
  EXPECT_EQ(engine.latest().values.ofi_200, 2);

  const auto improved_ask = event(2, tickbook::Side::Ask, 101, 2, tickbook::EventType::BookUpdate);
  book.apply(improved_ask);
  EXPECT_EQ(engine.on_event(improved_ask, book), tickbook::FeatureUpdateResult::Updated);
  EXPECT_EQ(engine.latest().values.ofi_10, 0);
}

TEST(FeatureEngine, KeepsIndependentTenFiftyAndTwoHundredMessageOfiWindows) {
  tickbook::FeatureEngine engine({1, 128});
  tickbook::BookMap book;
  baseline(engine, book);
  for (std::int64_t update = 1; update <= 12; ++update) {
    const auto change =
        event(update, tickbook::Side::Bid, 100, 3 + update, tickbook::EventType::BookUpdate);
    book.apply(change);
    ASSERT_EQ(engine.on_event(change, book), tickbook::FeatureUpdateResult::Updated);
  }
  EXPECT_EQ(engine.latest().values.ofi_10, 10);
  EXPECT_EQ(engine.latest().values.ofi_50, 12);
  EXPECT_EQ(engine.latest().values.ofi_200, 12);

  for (std::int64_t update = 13; update <= 60; ++update) {
    const auto change =
        event(update, tickbook::Side::Bid, 100, 3 + update, tickbook::EventType::BookUpdate);
    book.apply(change);
    ASSERT_EQ(engine.on_event(change, book), tickbook::FeatureUpdateResult::Updated);
  }
  EXPECT_EQ(engine.latest().values.ofi_10, 10);
  EXPECT_EQ(engine.latest().values.ofi_50, 50);
  EXPECT_EQ(engine.latest().values.ofi_200, 60);
}

TEST(FeatureEngine, ComputesTradeFlowAndMidReturnsOnEventTimeWindows) {
  tickbook::FeatureEngine engine({1, 8});
  tickbook::BookMap book;
  baseline(engine, book);

  const auto buy = event(50'000'000, tickbook::Side::Bid, 100, 7, tickbook::EventType::Trade);
  book.apply(buy);
  EXPECT_EQ(engine.on_event(buy, book), tickbook::FeatureUpdateResult::Updated);
  EXPECT_EQ(engine.latest().values.trade_flow_100ms, 7);

  const auto remove_ask =
      event(100'000'000, tickbook::Side::Ask, 102, 0, tickbook::EventType::BookUpdate, 0);
  book.apply(remove_ask);
  EXPECT_EQ(engine.on_event(remove_ask, book), tickbook::FeatureUpdateResult::MessagePending);
  const auto move_ask =
      event(100'000'000, tickbook::Side::Ask, 104, 1, tickbook::EventType::BookUpdate, 0);
  book.apply(move_ask);
  EXPECT_EQ(engine.on_event(move_ask, book), tickbook::FeatureUpdateResult::MessagePending);
  const auto move_bid =
      event(100'000'000, tickbook::Side::Bid, 102, 3, tickbook::EventType::BookUpdate, 0);
  book.apply(move_bid);
  EXPECT_EQ(engine.on_event(move_bid, book), tickbook::FeatureUpdateResult::MessagePending);
  const auto sell = event(100'000'000, tickbook::Side::Ask, 102, 3, tickbook::EventType::Trade);
  book.apply(sell);
  auto end_message = sell;
  end_message.flags = tickbook::kEventFlagEndOfMessage;
  EXPECT_EQ(engine.on_event(end_message, book), tickbook::FeatureUpdateResult::Updated);
  EXPECT_EQ(engine.latest().values.trade_flow_100ms, 4);
  EXPECT_EQ(engine.latest().values.trade_flow_1s, 4);
  EXPECT_EQ(engine.latest().values.mid_return_100ms_q20_ticks, 2 * tickbook::kFeatureScale);
  EXPECT_EQ(engine.latest().values.mid_return_1s_q20_ticks, 2 * tickbook::kFeatureScale);

  const auto later =
      event(151'000'000, tickbook::Side::Ask, 102, 3, tickbook::EventType::BookUpdate);
  book.apply(later);
  EXPECT_EQ(engine.on_event(later, book), tickbook::FeatureUpdateResult::Updated);
  EXPECT_EQ(engine.latest().values.trade_flow_100ms, -3);
  EXPECT_EQ(engine.latest().values.trade_flow_1s, 4);
}

TEST(FeatureEngine, ReturnsZeroWhenWindowHasNoSamplesEvenWhenRingWraps) {
  tickbook::FeatureEngine engine({1, 2});
  tickbook::BookMap book;
  const auto snapshot_bid =
      event(0, tickbook::Side::Bid, 100, 3, tickbook::EventType::SnapshotLevel,
            tickbook::kEventFlagSnapshotStart);
  EXPECT_EQ(apply(engine, book, snapshot_bid).values.mid_return_100ms_q20_ticks, 0);
  apply(engine, book, event(0, tickbook::Side::Ask, 102, 1, tickbook::EventType::SnapshotLevel));

  const auto remove_initial_ask =
      event(2'000'000'000, tickbook::Side::Ask, 102, 0, tickbook::EventType::BookUpdate, 0);
  apply(engine, book, remove_initial_ask);
  apply(engine, book,
        event(2'000'000'000, tickbook::Side::Ask, 104, 1, tickbook::EventType::BookUpdate));

  const auto remove_moved_ask =
      event(2'500'000'000, tickbook::Side::Ask, 104, 0, tickbook::EventType::BookUpdate, 0);
  apply(engine, book, remove_moved_ask);
  apply(engine, book,
        event(2'500'000'000, tickbook::Side::Ask, 106, 1, tickbook::EventType::BookUpdate));

  EXPECT_EQ(engine.latest().values.mid_return_100ms_q20_ticks, 0);
  EXPECT_EQ(engine.latest().values.mid_return_1s_q20_ticks, tickbook::kFeatureScale);

  apply(engine, book,
        event(4'000'000'000, tickbook::Side::Ask, 106, 0, tickbook::EventType::BookUpdate, 0));
  apply(engine, book,
        event(4'000'000'000, tickbook::Side::Ask, 108, 1, tickbook::EventType::BookUpdate));
  EXPECT_EQ(engine.latest().values.mid_return_100ms_q20_ticks, 0);
  EXPECT_EQ(engine.latest().values.mid_return_1s_q20_ticks, 0);
}

TEST(FeatureEngine, AGapLongerThanOneHundredMillisecondsHasNoHundredMillisecondReturn) {
  tickbook::FeatureEngine engine({1, 8});
  tickbook::BookMap book;
  baseline(engine, book);

  const auto moved_ask =
      event(200'000'000, tickbook::Side::Ask, 101, 1, tickbook::EventType::BookUpdate);
  book.apply(moved_ask);
  ASSERT_EQ(engine.on_event(moved_ask, book), tickbook::FeatureUpdateResult::Updated);

  EXPECT_EQ(engine.latest().values.mid_return_100ms_q20_ticks, 0);
  EXPECT_EQ(engine.latest().values.mid_return_1s_q20_ticks, -tickbook::kFeatureScale / 2);
}

TEST(FeatureEngine, ReportsWindowCapacityAndRejectsBackwardsEventTime) {
  tickbook::FeatureEngine engine({1, 1});
  tickbook::BookMap book;
  baseline(engine, book);
  const auto same_time = event(0, tickbook::Side::Bid, 100, 3, tickbook::EventType::BookUpdate);
  book.apply(same_time);
  EXPECT_EQ(engine.on_event(same_time, book),
            tickbook::FeatureUpdateResult::TimeWindowCapacityExceeded);
  EXPECT_EQ(engine.window_capacity_errors(), 1U);

  engine.reset();
  const auto later = event(10, tickbook::Side::Bid, 100, 3, tickbook::EventType::BookUpdate);
  book.apply(later);
  EXPECT_EQ(engine.on_event(later, book), tickbook::FeatureUpdateResult::Updated);
  const auto earlier = event(9, tickbook::Side::Bid, 100, 4, tickbook::EventType::BookUpdate);
  book.apply(earlier);
  EXPECT_EQ(engine.on_event(earlier, book), tickbook::FeatureUpdateResult::InvalidTimestamp);
}

TEST(FeatureEngine, RejectsNonPowerOfTwoWindowCapacity) {
  tickbook::FeatureEngine engine({1, 3});
  tickbook::BookMap book;
  const auto update = event(1, tickbook::Side::Bid, 100, 3, tickbook::EventType::BookUpdate);
  EXPECT_EQ(engine.on_event(update, book), tickbook::FeatureUpdateResult::InvalidConfiguration);
}
