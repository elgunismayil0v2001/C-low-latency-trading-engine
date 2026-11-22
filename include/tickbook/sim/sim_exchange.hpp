#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "tickbook/books/order_book.hpp"
#include "tickbook/sim/metrics.hpp"
#include "tickbook/sim/strategy.hpp"

namespace tickbook::sim {

inline constexpr std::uint32_t kDefaultTakerFeePpm = 1'000; // 0.10% regular-user Spot fee.
inline constexpr std::size_t kSimOrderCapacity = 1'024;
inline constexpr std::uint64_t kQuoteFractionScale = 1'000'000;

struct SimExchangeConfig {
  std::int64_t latency_ns{5'000'000};
  std::uint32_t fee_ppm{kDefaultTakerFeePpm};
  std::int32_t qty_scale{8};
  Qty max_order_qty{};
  Qty max_position_qty{};
};

enum class ExchangeStatus : std::uint8_t {
  Ok,
  InvalidConfiguration,
  InvalidIntent,
  PositionLimit,
  QueueFull,
  TimeWentBackwards,
  TimestampOverflow,
};

namespace exchange_detail {

struct MulDivResult {
  std::uint64_t quotient{};
  std::uint64_t remainder{};
  bool overflowed{};
};

[[nodiscard]] inline MulDivResult mul_div(std::uint64_t left, std::uint64_t right,
                                          std::uint64_t divisor) noexcept {
  if (divisor == 0) return {std::numeric_limits<std::uint64_t>::max(), 0, true};
  if (right == 0 || left <= std::numeric_limits<std::uint64_t>::max() / right) {
    const auto product = left * right;
    return {product / divisor, product % divisor, false};
  }

  constexpr auto kMask = std::uint64_t{0xffff'ffffU};
  const auto left_low = left & kMask;
  const auto left_high = left >> 32U;
  const auto right_low = right & kMask;
  const auto right_high = right >> 32U;
  const auto word0 = left_low * right_low;
  auto middle = left_high * right_low + (word0 >> 32U);
  const auto word1 = middle & kMask;
  const auto word2 = middle >> 32U;
  middle = left_low * right_high + word1;
  const auto high = left_high * right_high + word2 + (middle >> 32U);
  const auto low = (middle << 32U) | (word0 & kMask);

  if (high >= divisor) return {std::numeric_limits<std::uint64_t>::max(), 0, true};
  auto remainder = high;
  std::uint64_t quotient = 0;
  for (int bit = 63; bit >= 0; --bit) {
    const auto input_bit = (low >> static_cast<unsigned int>(bit)) & 1U;
    bool quotient_bit = false;
    if (remainder >= divisor - remainder) {
      remainder -= divisor - remainder;
      remainder += input_bit;
      quotient_bit = true;
    } else {
      remainder += remainder;
      if (input_bit != 0 && remainder == divisor - 1U) {
        remainder = 0;
        quotient_bit = true;
      } else {
        remainder += input_bit;
      }
    }
    quotient = (quotient << 1U) | static_cast<std::uint64_t>(quotient_bit);
  }
  return {quotient, remainder, false};
}

[[nodiscard]] inline std::int64_t signed_saturate(std::uint64_t value, bool& overflowed) noexcept {
  const auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (value > limit) {
    overflowed = true;
    return std::numeric_limits<std::int64_t>::max();
  }
  return static_cast<std::int64_t>(value);
}

[[nodiscard]] inline std::uint64_t power_of_ten(std::int32_t exponent) noexcept {
  std::uint64_t result = 1;
  for (std::int32_t index = 0; index < exponent; ++index) result *= 10U;
  return result;
}

} // namespace exchange_detail

// Market orders activate against the first complete message at or after their due
// time. The recorded book is read only, so fills do not change later replay depth.
class SimExchange {
public:
  explicit SimExchange(SimExchangeConfig config) noexcept : config_(config) {
    valid_ = config_.latency_ns >= 0 && config_.fee_ppm <= 1'000'000 && config_.qty_scale >= 0 &&
             config_.qty_scale <= 18 && config_.max_order_qty > 0 && config_.max_position_qty > 0 &&
             config_.max_order_qty <= config_.max_position_qty;
    if (valid_) qty_scale_factor_ = exchange_detail::power_of_ten(config_.qty_scale);
  }

  void reset() noexcept {
    queue_head_ = 0;
    queue_size_ = 0;
    position_ = 0;
    projected_position_ = 0;
    gross_cash_ = 0;
    net_cash_ = 0;
    cycle_pnl_ = 0;
    cycle_entry_notional_ = 0;
    have_time_ = false;
    last_time_ns_ = 0;
    overflowed_ = false;
    metrics_.reset();
  }

  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] Qty position() const noexcept { return position_; }
  [[nodiscard]] std::size_t pending_orders() const noexcept { return queue_size_; }
  [[nodiscard]] std::uint32_t fee_ppm() const noexcept { return config_.fee_ppm; }
  [[nodiscard]] std::int64_t latency_ns() const noexcept { return config_.latency_ns; }
  [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
  [[nodiscard]] const Metrics& metrics() const noexcept { return metrics_; }
  void record_latency(std::uint64_t elapsed_ns) noexcept { metrics_.record_latency(elapsed_ns); }

  [[nodiscard]] Qty projected_position() const noexcept { return projected_position_; }

  [[nodiscard]] ExchangeStatus submit(const OrderIntent& intent,
                                      std::int64_t submitted_at_ns) noexcept {
    metrics_.record_order_submitted();
    if (!valid_) {
      metrics_.record_order_rejected();
      return ExchangeStatus::InvalidConfiguration;
    }
    if (submitted_at_ns < 0 || (have_time_ && submitted_at_ns < last_time_ns_)) {
      metrics_.record_order_rejected();
      return ExchangeStatus::TimeWentBackwards;
    }
    have_time_ = true;
    last_time_ns_ = submitted_at_ns;
    if ((intent.side != OrderSide::Buy && intent.side != OrderSide::Sell) || intent.qty <= 0 ||
        intent.qty > config_.max_order_qty) {
      metrics_.record_order_rejected();
      return ExchangeStatus::InvalidIntent;
    }
    if (queue_size_ == queue_.size()) {
      metrics_.record_order_rejected();
      return ExchangeStatus::QueueFull;
    }
    const auto projected = projected_position();
    if (intent.side == OrderSide::Buy) {
      if (projected < 0 || intent.qty > config_.max_position_qty - projected) {
        metrics_.record_order_rejected();
        return ExchangeStatus::PositionLimit;
      }
    } else if (intent.qty > projected) {
      metrics_.record_order_rejected();
      return ExchangeStatus::PositionLimit;
    }
    if (config_.latency_ns > std::numeric_limits<std::int64_t>::max() - submitted_at_ns) {
      metrics_.record_order_rejected();
      return ExchangeStatus::TimestampOverflow;
    }
    const auto tail = (queue_head_ + queue_size_) % queue_.size();
    queue_[tail] = ScheduledOrder{intent.side, intent.qty, submitted_at_ns + config_.latency_ns};
    ++queue_size_;
    if (intent.side == OrderSide::Buy) projected_position_ += intent.qty;
    else projected_position_ -= intent.qty;
    metrics_.record_order_accepted();
    return ExchangeStatus::Ok;
  }

  template <OrderBook Book>
  [[nodiscard]] ExchangeStatus process_due(std::int64_t now_ns, const Book& book) noexcept {
    if (!valid_) return ExchangeStatus::InvalidConfiguration;
    if (now_ns < 0 || (have_time_ && now_ns < last_time_ns_))
      return ExchangeStatus::TimeWentBackwards;
    have_time_ = true;
    last_time_ns_ = now_ns;
    while (queue_size_ != 0 && queue_[queue_head_].due_ns <= now_ns) {
      const auto order = queue_[queue_head_];
      queue_head_ = (queue_head_ + 1U) % queue_.size();
      --queue_size_;
      execute(order, book);
    }
    return ExchangeStatus::Ok;
  }

  template <OrderBook Book> void mark_to_market(const Book& book) noexcept {
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    if (!bid || !ask || bid->price < 0 || ask->price < 0) return;
    const auto mid2 =
        static_cast<std::uint64_t>(bid->price) + static_cast<std::uint64_t>(ask->price);
    const auto divisor = qty_scale_factor_ * 2U;
    const auto marked = quote_notional(mid2, static_cast<std::uint64_t>(position_), divisor);
    const auto gross = add_saturating(gross_cash_, marked);
    const auto net = add_saturating(net_cash_, marked);
    metrics_.observe_equity(gross, net);
  }

  void cancel_pending() noexcept {
    while (queue_size_ != 0) {
      const auto order = queue_[queue_head_];
      metrics_.record_unfilled(static_cast<std::uint64_t>(order.qty));
      if (order.side == OrderSide::Buy) projected_position_ -= order.qty;
      else projected_position_ += order.qty;
      queue_head_ = (queue_head_ + 1U) % queue_.size();
      --queue_size_;
    }
  }

private:
  struct ScheduledOrder {
    OrderSide side{OrderSide::Buy};
    Qty qty{};
    std::int64_t due_ns{};
  };

  [[nodiscard]] std::int64_t add_saturating(std::int64_t left, std::int64_t right) noexcept {
    bool local_overflow = false;
    metrics_detail::add_saturating(left, right, local_overflow);
    overflowed_ = overflowed_ || local_overflow;
    return left;
  }

  [[nodiscard]] std::int64_t notional(Price price, Qty qty) noexcept {
    return quote_notional(static_cast<std::uint64_t>(price), static_cast<std::uint64_t>(qty),
                          qty_scale_factor_);
  }

  [[nodiscard]] std::int64_t quote_notional(std::uint64_t price_units, std::uint64_t quantity_units,
                                            std::uint64_t divisor) noexcept {
    const auto whole = exchange_detail::mul_div(price_units, quantity_units, divisor);
    overflowed_ = overflowed_ || whole.overflowed;
    if (whole.overflowed ||
        whole.quotient > std::numeric_limits<std::uint64_t>::max() / kQuoteFractionScale) {
      overflowed_ = true;
      return std::numeric_limits<std::int64_t>::max();
    }
    const auto fractional = exchange_detail::mul_div(whole.remainder, kQuoteFractionScale, divisor);
    overflowed_ = overflowed_ || fractional.overflowed;
    if (fractional.overflowed ||
        whole.quotient * kQuoteFractionScale >
            std::numeric_limits<std::uint64_t>::max() - fractional.quotient) {
      overflowed_ = true;
      return std::numeric_limits<std::int64_t>::max();
    }
    const auto total = whole.quotient * kQuoteFractionScale + fractional.quotient;
    return exchange_detail::signed_saturate(total, overflowed_);
  }

  [[nodiscard]] std::int64_t fee_for(std::int64_t value) noexcept {
    if (config_.fee_ppm == 0 || value <= 0) return 0;
    const auto result =
        exchange_detail::mul_div(static_cast<std::uint64_t>(value), config_.fee_ppm, 1'000'000U);
    overflowed_ = overflowed_ || result.overflowed;
    auto fee = result.quotient;
    if (result.remainder != 0 && fee != std::numeric_limits<std::uint64_t>::max()) ++fee;
    return exchange_detail::signed_saturate(fee, overflowed_);
  }

  void execute(const ScheduledOrder& order, const auto& book) noexcept {
    Qty remaining = order.qty;
    bool did_fill = false;
    const auto fill = [&](Price price, Qty available) noexcept {
      if (remaining == 0) return false;
      const auto quantity = std::min(remaining, available);
      if (quantity <= 0) return true;
      const auto value = notional(price, quantity);
      const auto fee = fee_for(value);
      metrics_.record_fill(value, fee);
      if (order.side == OrderSide::Buy) {
        position_ += quantity;
        gross_cash_ = add_saturating(gross_cash_, -value);
        net_cash_ = add_saturating(net_cash_, add_saturating(-value, -fee));
        cycle_pnl_ = add_saturating(cycle_pnl_, add_saturating(-value, -fee));
        cycle_entry_notional_ = add_saturating(cycle_entry_notional_, value);
      } else {
        position_ -= quantity;
        gross_cash_ = add_saturating(gross_cash_, value);
        net_cash_ = add_saturating(net_cash_, add_saturating(value, -fee));
        cycle_pnl_ = add_saturating(cycle_pnl_, add_saturating(value, -fee));
      }
      remaining -= quantity;
      did_fill = true;
      return remaining != 0;
    };
    if (order.side == OrderSide::Buy) book.visit_asks(fill);
    else book.visit_bids(fill);
    if (did_fill) metrics_.record_order_filled();
    if (remaining != 0) metrics_.record_unfilled(static_cast<std::uint64_t>(remaining));
    if (order.side == OrderSide::Buy) projected_position_ -= remaining;
    else projected_position_ += remaining;
    if (position_ == 0 && did_fill) {
      metrics_.record_closed_trade(cycle_pnl_, cycle_entry_notional_);
      cycle_pnl_ = 0;
      cycle_entry_notional_ = 0;
    }
  }

  SimExchangeConfig config_{};
  std::array<ScheduledOrder, kSimOrderCapacity> queue_{};
  std::size_t queue_head_{};
  std::size_t queue_size_{};
  std::uint64_t qty_scale_factor_{1};
  Qty position_{};
  Qty projected_position_{};
  std::int64_t gross_cash_{};
  std::int64_t net_cash_{};
  std::int64_t cycle_pnl_{};
  std::int64_t cycle_entry_notional_{};
  std::int64_t last_time_ns_{};
  bool have_time_{};
  bool valid_{};
  bool overflowed_{};
  Metrics metrics_{};
};

} // namespace tickbook::sim
