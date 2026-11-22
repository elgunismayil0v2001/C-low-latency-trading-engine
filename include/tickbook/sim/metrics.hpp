#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

#include "tickbook/latency_histogram.hpp"

namespace tickbook::sim {

struct MetricsSnapshot {
  std::int64_t gross_pnl{};
  std::int64_t net_pnl{};
  std::int64_t fees{};
  std::int64_t turnover{};
  std::int64_t maximum_drawdown{};
  std::int64_t average_trade_pnl_bps{};
  std::int64_t break_even_fee_bps{};
  std::uint64_t orders_submitted{};
  std::uint64_t orders_accepted{};
  std::uint64_t orders_rejected{};
  std::uint64_t orders_filled{};
  std::uint64_t fills{};
  std::uint64_t unfilled_qty{};
  std::uint64_t closed_trades{};
  std::uint64_t winning_trades{};
  std::uint32_t hit_rate_bps{};
  bool has_trade_statistics{};
  bool overflowed{};
};

namespace metrics_detail {

inline void add_saturating(std::int64_t& destination, std::int64_t value,
                           bool& overflowed) noexcept {
  if (value > 0 && destination > std::numeric_limits<std::int64_t>::max() - value) {
    destination = std::numeric_limits<std::int64_t>::max();
    overflowed = true;
  } else if (value < 0 && destination < std::numeric_limits<std::int64_t>::min() - value) {
    destination = std::numeric_limits<std::int64_t>::min();
    overflowed = true;
  } else {
    destination += value;
  }
}

inline void increment_saturating(std::uint64_t& value, bool& overflowed) noexcept {
  if (value == std::numeric_limits<std::uint64_t>::max()) overflowed = true;
  else ++value;
}

[[nodiscard]] inline std::uint64_t magnitude(std::int64_t value) noexcept {
  return value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1U
                   : static_cast<std::uint64_t>(value);
}

[[nodiscard]] inline std::uint64_t fraction_q20(std::uint64_t numerator,
                                                std::uint64_t denominator) noexcept {
  if (denominator == 0) return 0;
  if (numerator >= denominator) return 1U << 20U;
  std::uint64_t remainder = numerator;
  std::uint64_t quotient = 0;
  for (unsigned int bit = 0; bit < 20; ++bit) {
    quotient <<= 1U;
    if (remainder >= denominator - remainder) {
      remainder -= denominator - remainder;
      quotient |= 1U;
    } else {
      remainder += remainder;
    }
  }
  return quotient;
}

[[nodiscard]] inline std::uint64_t
fraction_scaled(std::uint64_t numerator, std::uint64_t denominator, std::uint64_t scale) noexcept {
  if (denominator == 0 || scale == 0) return 0;
  if (numerator >= denominator) return scale;
  std::uint64_t low = 0;
  std::uint64_t high = scale;
  while (low < high) {
    const auto candidate = low + (high - low + 1U) / 2U;
    const auto whole = (denominator / scale) * candidate;
    const auto fractional_product = (denominator % scale) * candidate;
    const auto threshold = whole + fractional_product / scale +
                           static_cast<std::uint64_t>(fractional_product % scale != 0);
    if (numerator >= threshold) low = candidate;
    else high = candidate - 1U;
  }
  return low;
}

[[nodiscard]] inline std::int64_t ratio_bps(std::int64_t numerator,
                                            std::int64_t denominator) noexcept {
  if (denominator <= 0) return 0;
  const bool negative = numerator < 0;
  const auto absolute = magnitude(numerator);
  const auto divisor = static_cast<std::uint64_t>(denominator);
  const auto whole = absolute / divisor;
  const auto remainder = absolute % divisor;
  constexpr auto kBpsScale = std::uint64_t{10'000};
  constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (whole > kMax / kBpsScale) {
    return negative ? std::numeric_limits<std::int64_t>::min()
                    : std::numeric_limits<std::int64_t>::max();
  }
  const auto base = whole * kBpsScale;
  const auto fraction = fraction_scaled(remainder, divisor, kBpsScale);
  if (fraction > kMax - base) {
    return negative ? std::numeric_limits<std::int64_t>::min()
                    : std::numeric_limits<std::int64_t>::max();
  }
  const auto result = base + fraction;
  if (negative) {
    if (result >= (std::uint64_t{1} << 63U)) return std::numeric_limits<std::int64_t>::min();
    return -static_cast<std::int64_t>(result);
  }
  return static_cast<std::int64_t>(result);
}

[[nodiscard]] inline std::int64_t positive_difference(std::int64_t high,
                                                      std::int64_t low) noexcept {
  if (high <= low) return 0;
  const auto difference = static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low);
  return difference > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
             ? std::numeric_limits<std::int64_t>::max()
             : static_cast<std::int64_t>(difference);
}

} // namespace metrics_detail

class Metrics {
public:
  void reset() noexcept {
    snapshot_ = {};
    trade_bps_sum_ = 0;
    high_water_net_pnl_ = 0;
    latency_.reset();
  }

  void record_order_submitted() noexcept {
    metrics_detail::increment_saturating(snapshot_.orders_submitted, snapshot_.overflowed);
  }

  void record_order_accepted() noexcept {
    metrics_detail::increment_saturating(snapshot_.orders_accepted, snapshot_.overflowed);
  }

  void record_order_rejected() noexcept {
    metrics_detail::increment_saturating(snapshot_.orders_rejected, snapshot_.overflowed);
  }

  void record_order_filled() noexcept {
    metrics_detail::increment_saturating(snapshot_.orders_filled, snapshot_.overflowed);
  }

  void record_fill(std::int64_t notional, std::int64_t fee) noexcept {
    metrics_detail::increment_saturating(snapshot_.fills, snapshot_.overflowed);
    metrics_detail::add_saturating(snapshot_.turnover, notional, snapshot_.overflowed);
    metrics_detail::add_saturating(snapshot_.fees, fee, snapshot_.overflowed);
  }

  void record_unfilled(std::uint64_t quantity) noexcept {
    if (quantity > std::numeric_limits<std::uint64_t>::max() - snapshot_.unfilled_qty) {
      snapshot_.unfilled_qty = std::numeric_limits<std::uint64_t>::max();
      snapshot_.overflowed = true;
    } else {
      snapshot_.unfilled_qty += quantity;
    }
  }

  void record_closed_trade(std::int64_t net_pnl, std::int64_t cycle_turnover) noexcept {
    metrics_detail::increment_saturating(snapshot_.closed_trades, snapshot_.overflowed);
    if (net_pnl > 0) {
      metrics_detail::increment_saturating(snapshot_.winning_trades, snapshot_.overflowed);
    }
    metrics_detail::add_saturating(
        trade_bps_sum_, metrics_detail::ratio_bps(net_pnl, cycle_turnover), snapshot_.overflowed);
  }

  void observe_equity(std::int64_t gross_pnl, std::int64_t net_pnl) noexcept {
    snapshot_.gross_pnl = gross_pnl;
    snapshot_.net_pnl = net_pnl;
    if (net_pnl > high_water_net_pnl_) high_water_net_pnl_ = net_pnl;
    const auto drawdown = metrics_detail::positive_difference(high_water_net_pnl_, net_pnl);
    snapshot_.maximum_drawdown = std::max(snapshot_.maximum_drawdown, drawdown);
  }

  void record_latency(std::uint64_t elapsed_ns) noexcept { latency_.record(elapsed_ns); }

  [[nodiscard]] MetricsSnapshot snapshot() const noexcept {
    auto result = snapshot_;
    if (result.closed_trades != 0) {
      result.has_trade_statistics = true;
      result.average_trade_pnl_bps =
          result.closed_trades >
                  static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
              ? 0
              : trade_bps_sum_ / static_cast<std::int64_t>(result.closed_trades);
      result.hit_rate_bps = static_cast<std::uint32_t>(
          metrics_detail::fraction_q20(result.winning_trades, result.closed_trades) * 10'000U /
          (1U << 20U));
    }
    if (result.turnover > 0) {
      result.break_even_fee_bps = metrics_detail::ratio_bps(result.gross_pnl, result.turnover);
    }
    return result;
  }

  [[nodiscard]] const LatencyHistogram& latency_histogram() const noexcept { return latency_; }

private:
  MetricsSnapshot snapshot_{};
  std::int64_t trade_bps_sum_{};
  std::int64_t high_water_net_pnl_{};
  LatencyHistogram latency_{};
};

} // namespace tickbook::sim
