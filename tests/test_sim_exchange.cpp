#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "tickbook/books/offset_book.hpp"
#include "tickbook/sim/sim_exchange.hpp"

namespace {

tickbook::Event level(tickbook::Side side, tickbook::Price price, tickbook::Qty qty) {
  tickbook::Event result{};
  result.type = tickbook::EventType::SnapshotLevel;
  result.side = side;
  result.price = price;
  result.qty = qty;
  return result;
}

tickbook::OffsetArrayBook two_sided_book() {
  tickbook::OffsetArrayBook book(16);
  book.apply(level(tickbook::Side::Bid, 100, 3));
  book.apply(level(tickbook::Side::Ask, 102, 3));
  return book;
}

tickbook::sim::SimExchangeConfig config(std::int64_t latency_ns = 10,
                                        std::uint32_t fee_ppm = 1'000) {
  return {latency_ns, fee_ppm, 0, 2, 4};
}

} // namespace

TEST(SimExchange, ActivatesOrdersAtConfiguredTimeAndMarksLongPositionToMid) {
  auto book = two_sided_book();
  tickbook::sim::SimExchange exchange(config());
  ASSERT_TRUE(exchange.valid());
  ASSERT_EQ(exchange.submit({tickbook::sim::OrderSide::Buy, 2}, 100),
            tickbook::sim::ExchangeStatus::Ok);
  EXPECT_EQ(exchange.process_due(109, book), tickbook::sim::ExchangeStatus::Ok);
  EXPECT_EQ(exchange.position(), 0);
  EXPECT_EQ(exchange.process_due(110, book), tickbook::sim::ExchangeStatus::Ok);
  EXPECT_EQ(exchange.position(), 2);
  EXPECT_EQ(exchange.projected_position(), 2);
  ASSERT_TRUE(book.best_ask().has_value());
  EXPECT_EQ(book.best_ask()->qty, 3); // Recorded depth is not mutated by a simulated fill.
  exchange.mark_to_market(book);
  const auto metrics = exchange.metrics().snapshot();
  EXPECT_EQ(metrics.turnover, 204'000'000);
  EXPECT_EQ(metrics.fees, 204'000);
  EXPECT_EQ(metrics.gross_pnl, -2'000'000);
  EXPECT_EQ(metrics.net_pnl, -2'204'000);
  EXPECT_EQ(metrics.orders_filled, 1U);
}

TEST(SimExchange, WalksLevelsAndClosesTradeAfterMarketSell) {
  tickbook::OffsetArrayBook book(16);
  book.apply(level(tickbook::Side::Bid, 100, 1));
  book.apply(level(tickbook::Side::Bid, 99, 2));
  book.apply(level(tickbook::Side::Ask, 101, 1));
  book.apply(level(tickbook::Side::Ask, 102, 2));
  tickbook::sim::SimExchange exchange(config(0, 0));
  ASSERT_EQ(exchange.submit({tickbook::sim::OrderSide::Buy, 2}, 0),
            tickbook::sim::ExchangeStatus::Ok);
  ASSERT_EQ(exchange.process_due(0, book), tickbook::sim::ExchangeStatus::Ok);
  EXPECT_EQ(exchange.position(), 2);
  EXPECT_EQ(exchange.metrics().snapshot().fills, 2U);
  ASSERT_EQ(exchange.submit({tickbook::sim::OrderSide::Sell, 2}, 1),
            tickbook::sim::ExchangeStatus::Ok);
  ASSERT_EQ(exchange.process_due(1, book), tickbook::sim::ExchangeStatus::Ok);
  exchange.mark_to_market(book);
  EXPECT_EQ(exchange.position(), 0);
  const auto result = exchange.metrics().snapshot();
  EXPECT_EQ(result.closed_trades, 1U);
  EXPECT_EQ(result.orders_filled, 2U);
  EXPECT_EQ(result.gross_pnl, -4'000'000);
  EXPECT_EQ(result.net_pnl, -4'000'000);
}

TEST(SimExchange, RejectsOrdersThatExceedTheLongOnlyPositionLimit) {
  auto book = two_sided_book();
  tickbook::sim::SimExchange exchange({0, 0, 0, 3, 3});
  EXPECT_EQ(exchange.submit({tickbook::sim::OrderSide::Buy, 4}, 0),
            tickbook::sim::ExchangeStatus::InvalidIntent);
  ASSERT_EQ(exchange.submit({tickbook::sim::OrderSide::Buy, 2}, 0),
            tickbook::sim::ExchangeStatus::Ok);
  ASSERT_EQ(exchange.process_due(0, book), tickbook::sim::ExchangeStatus::Ok);
  EXPECT_EQ(exchange.submit({tickbook::sim::OrderSide::Buy, 2}, 1),
            tickbook::sim::ExchangeStatus::PositionLimit);
  EXPECT_EQ(exchange.submit({tickbook::sim::OrderSide::Sell, 3}, 1),
            tickbook::sim::ExchangeStatus::PositionLimit);
  EXPECT_EQ(exchange.metrics().snapshot().orders_rejected, 3U);
}

TEST(SimExchange, CancelsUnfilledMarketQuantityAndPendingOrdersAtEndOfReplay) {
  tickbook::OffsetArrayBook book(8);
  book.apply(level(tickbook::Side::Bid, 100, 1));
  book.apply(level(tickbook::Side::Ask, 102, 1));
  tickbook::sim::SimExchange exchange(config(0, 0));
  ASSERT_EQ(exchange.submit({tickbook::sim::OrderSide::Buy, 2}, 0),
            tickbook::sim::ExchangeStatus::Ok);
  ASSERT_EQ(exchange.process_due(0, book), tickbook::sim::ExchangeStatus::Ok);
  EXPECT_EQ(exchange.position(), 1);
  EXPECT_EQ(exchange.projected_position(), 1);
  EXPECT_EQ(exchange.metrics().snapshot().unfilled_qty, 1U);

  ASSERT_EQ(exchange.submit({tickbook::sim::OrderSide::Sell, 1}, 1),
            tickbook::sim::ExchangeStatus::Ok);
  EXPECT_EQ(exchange.projected_position(), 0);
  exchange.cancel_pending();
  EXPECT_EQ(exchange.pending_orders(), 0U);
  EXPECT_EQ(exchange.projected_position(), 1);
  EXPECT_EQ(exchange.metrics().snapshot().unfilled_qty, 2U);
}

TEST(SimExchange, UsesExactWideMultiplyDivideForScaledNotional) {
  constexpr auto kMaximum = std::numeric_limits<std::uint64_t>::max();
  const auto result = tickbook::sim::exchange_detail::mul_div(
      1'000'000'000'000'000'000ULL, 1'000'000'000'000'000'000ULL, 1'000'000'000'000'000'000ULL);
  EXPECT_FALSE(result.overflowed);
  EXPECT_EQ(result.quotient, 1'000'000'000'000'000'000ULL);
  EXPECT_EQ(tickbook::sim::exchange_detail::mul_div(kMaximum, kMaximum, kMaximum).quotient,
            kMaximum);
  EXPECT_TRUE(tickbook::sim::exchange_detail::mul_div(kMaximum, kMaximum, 1).overflowed);
}
