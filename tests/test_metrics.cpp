#include <gtest/gtest.h>

#include <cstdint>

#include "tickbook/sim/metrics.hpp"

TEST(Metrics, TracksTradeAndFillStatisticsWithFixedPointRatios) {
  tickbook::sim::Metrics metrics;
  metrics.record_order_submitted();
  metrics.record_order_accepted();
  metrics.record_fill(10'000, 10);
  metrics.record_closed_trade(500, 10'000);
  metrics.record_closed_trade(-125, 5'000);

  const auto result = metrics.snapshot();
  EXPECT_EQ(result.orders_submitted, 1U);
  EXPECT_EQ(result.orders_accepted, 1U);
  EXPECT_EQ(result.fills, 1U);
  EXPECT_EQ(result.turnover, 10'000);
  EXPECT_EQ(result.fees, 10);
  EXPECT_EQ(result.closed_trades, 2U);
  EXPECT_EQ(result.winning_trades, 1U);
  EXPECT_EQ(result.average_trade_pnl_bps, 125);
  EXPECT_EQ(result.hit_rate_bps, 5'000U);
  EXPECT_EQ(result.break_even_fee_bps, 0);
}

TEST(Metrics, TracksNetDrawdownLatencyAndBreakEvenFee) {
  tickbook::sim::Metrics metrics;
  metrics.record_fill(10'000, 10);
  metrics.observe_equity(100, 90);
  metrics.observe_equity(150, 140);
  metrics.observe_equity(120, 110);
  metrics.record_latency(40);
  metrics.record_latency(90);

  const auto result = metrics.snapshot();
  EXPECT_EQ(result.gross_pnl, 120);
  EXPECT_EQ(result.net_pnl, 110);
  EXPECT_EQ(result.maximum_drawdown, 30);
  EXPECT_EQ(result.break_even_fee_bps, 120);
  EXPECT_EQ(metrics.latency_histogram().count(), 2U);
  EXPECT_EQ(metrics.latency_histogram().maximum_ns(), 90U);
}

TEST(Metrics, SaturatesCountersAndSignedArithmetic) {
  EXPECT_EQ(tickbook::sim::metrics_detail::ratio_bps(-1, 3), -3'333);
  EXPECT_EQ(tickbook::sim::metrics_detail::ratio_bps(1, 3), 3'333);
  EXPECT_EQ(tickbook::sim::metrics_detail::ratio_bps(3, 3), 10'000);
}
