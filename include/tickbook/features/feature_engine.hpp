#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "tickbook/books/order_book.hpp"

namespace tickbook {

inline constexpr std::int64_t kFeatureScale = 1'048'576;
inline constexpr std::size_t kFeatureTopK = 5;
inline constexpr std::size_t kFeatureMaxOfiWindow = 200;
inline constexpr std::size_t kDefaultFeatureTimeCapacity = 1U << 18U;
inline constexpr std::int64_t kFeatureWindow100MsNs = 100'000'000;
inline constexpr std::int64_t kFeatureWindow1SecNs = 1'000'000'000;

// Ratios and price returns use Q20 units; quantities remain in fixed-point units.
struct FeatureVector {
  std::int64_t spread_ticks{};
  std::int32_t imbalance_l1_q20{};
  std::int32_t imbalance_k_q20{};
  std::int64_t microprice_offset_q20_ticks{};
  std::int64_t ofi_10{};
  std::int64_t ofi_50{};
  std::int64_t ofi_200{};
  std::int64_t trade_flow_100ms{};
  std::int64_t trade_flow_1s{};
  std::int64_t mid_return_100ms_q20_ticks{};
  std::int64_t mid_return_1s_q20_ticks{};

  [[nodiscard]] bool operator==(const FeatureVector&) const noexcept = default;
};

struct FeatureSnapshot {
  std::int64_t ts_ns{};
  std::uint64_t mid2_price{};
  bool has_mid{};
  FeatureVector values{};
};

enum class FeatureUpdateResult : std::uint8_t {
  MessagePending,
  Updated,
  InvalidConfiguration,
  InvalidTimestamp,
  TimeWindowCapacityExceeded,
};

struct FeatureEngineConfig {
  Price price_tick{1};
  std::size_t time_window_capacity{kDefaultFeatureTimeCapacity};
};

namespace feature_detail {

class WideSigned {
public:
  void add(std::int64_t value) noexcept {
    const auto negative = value < 0;
    const auto magnitude = negative ? static_cast<std::uint64_t>(-(value + 1)) + 1U
                                    : static_cast<std::uint64_t>(value);
    add_magnitude(magnitude, negative);
  }

  void subtract(std::int64_t value) noexcept {
    const auto negative = value >= 0;
    const auto magnitude = value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1U
                                     : static_cast<std::uint64_t>(value);
    add_magnitude(magnitude, negative);
  }

  [[nodiscard]] std::int64_t as_int64() const noexcept {
    constexpr auto kNegativeLimit = std::uint64_t{1} << 63U;
    if (high_ != 0)
      return negative_ ? std::numeric_limits<std::int64_t>::min()
                       : std::numeric_limits<std::int64_t>::max();
    if (negative_) {
      if (low_ >= kNegativeLimit) return std::numeric_limits<std::int64_t>::min();
      return -static_cast<std::int64_t>(low_);
    }
    if (low_ > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(low_);
  }

private:
  void add_magnitude(std::uint64_t magnitude, bool negative) noexcept {
    if (magnitude == 0) return;
    if (low_ == 0 && high_ == 0) {
      low_ = magnitude;
      negative_ = negative;
      return;
    }
    if (negative_ == negative) {
      const auto previous = low_;
      low_ += magnitude;
      if (low_ < previous) ++high_;
      return;
    }
    if (high_ != 0 || low_ >= magnitude) {
      const auto previous = low_;
      low_ -= magnitude;
      if (previous < magnitude) --high_;
      if (low_ == 0 && high_ == 0) negative_ = false;
      return;
    }
    low_ = magnitude - low_;
    negative_ = negative;
  }

  std::uint64_t low_{};
  std::uint64_t high_{};
  bool negative_{};
};

[[nodiscard]] inline std::uint64_t magnitude(std::int64_t value) noexcept {
  return value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1U
                   : static_cast<std::uint64_t>(value);
}

[[nodiscard]] inline std::uint64_t fraction_q20(std::uint64_t numerator,
                                                std::uint64_t denominator) noexcept {
  if (denominator == 0) return 0;
  if (numerator >= denominator) return static_cast<std::uint64_t>(kFeatureScale);
  std::uint64_t remainder = numerator;
  std::uint64_t quotient = 0;
  for (std::size_t bit = 0; bit < 20; ++bit) {
    quotient <<= 1U;
    // This form doubles the remainder without overflowing when denominator is large.
    if (remainder >= denominator - remainder) {
      remainder -= denominator - remainder;
      quotient |= 1U;
    } else {
      remainder += remainder;
    }
  }
  return quotient;
}

[[nodiscard]] inline std::int64_t
signed_fraction_q20(std::uint64_t numerator, std::uint64_t denominator, bool negative) noexcept {
  const auto value = static_cast<std::int64_t>(fraction_q20(numerator, denominator));
  return negative ? -value : value;
}

struct PriceReturnInput {
  std::uint64_t current_mid2{};
  std::uint64_t previous_mid2{};
  std::uint64_t price_tick{};
};

[[nodiscard]] inline std::int64_t signed_price_return_q20(PriceReturnInput input) noexcept {
  const auto current_mid2 = input.current_mid2;
  const auto previous_mid2 = input.previous_mid2;
  const auto price_tick = input.price_tick;
  const bool negative = current_mid2 < previous_mid2;
  const auto difference = negative ? previous_mid2 - current_mid2 : current_mid2 - previous_mid2;
  const auto denominator = static_cast<std::uint64_t>(price_tick) * 2U;
  if (denominator == 0) return 0;
  const auto whole = difference / denominator;
  const auto remainder = difference % denominator;
  constexpr auto kScale = static_cast<std::uint64_t>(kFeatureScale);
  constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (whole > kMax / kScale) {
    return negative ? std::numeric_limits<std::int64_t>::min()
                    : std::numeric_limits<std::int64_t>::max();
  }
  const auto fractional = fraction_q20(remainder, denominator);
  const auto scaled = whole * kScale;
  if (fractional > kMax - scaled) {
    return negative ? std::numeric_limits<std::int64_t>::min()
                    : std::numeric_limits<std::int64_t>::max();
  }
  const auto result = static_cast<std::int64_t>(scaled + fractional);
  return negative ? -result : result;
}

[[nodiscard]] inline std::int64_t multiply_q20_half(std::int64_t ticks,
                                                    std::int64_t ratio_q20) noexcept {
  const bool negative = (ticks < 0) != (ratio_q20 < 0);
  const auto left = magnitude(ticks);
  const auto right = magnitude(ratio_q20);
  const auto whole = left / 2U;
  const auto remainder = left % 2U;
  constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (right != 0 && whole > kMax / right) {
    return negative ? std::numeric_limits<std::int64_t>::min()
                    : std::numeric_limits<std::int64_t>::max();
  }
  const auto product = whole * right + (remainder * right) / 2U;
  if (product > kMax) {
    return negative ? std::numeric_limits<std::int64_t>::min()
                    : std::numeric_limits<std::int64_t>::max();
  }
  const auto result = static_cast<std::int64_t>(product);
  return negative ? -result : result;
}

[[nodiscard]] inline std::uint64_t saturating_qty_add(std::uint64_t left,
                                                      std::uint64_t right) noexcept {
  constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<Qty>::max());
  return right > kMax - left ? kMax : left + right;
}

} // namespace feature_detail

// Called after each event has been applied to the book. It emits only at the end of
// a complete vendor message; all bounded time-window storage is allocated up front.
class FeatureEngine {
public:
  explicit FeatureEngine(FeatureEngineConfig config = {})
      : config_(config), time_samples_(config.time_window_capacity),
        valid_config_(config.price_tick > 0 && is_power_of_two(config.time_window_capacity)) {}

  template <OrderBook Book>
  [[nodiscard]] FeatureUpdateResult on_event(const Event& event, const Book& book) noexcept {
    if (!valid_config_) return FeatureUpdateResult::InvalidConfiguration;
    if ((event.flags & kEventFlagSnapshotStart) != 0) reset();
    if (event.ts_ns < 0 || (have_event_time_ && event.ts_ns < last_event_ts_ns_)) {
      return FeatureUpdateResult::InvalidTimestamp;
    }
    have_event_time_ = true;
    last_event_ts_ns_ = event.ts_ns;

    if (event.type == EventType::Trade && event.qty >= 0) {
      const auto signed_qty = event.side == Side::Bid ? event.qty : -event.qty;
      pending_trade_flow_.add(signed_qty);
    }
    if ((event.flags & kEventFlagEndOfMessage) == 0) return FeatureUpdateResult::MessagePending;

    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    const bool has_mid = bid && ask && bid->price >= 0 && ask->price >= 0;
    const auto mid2 =
        has_mid ? static_cast<std::uint64_t>(bid->price) + static_cast<std::uint64_t>(ask->price)
                : 0U;
    advance_time_windows(event.ts_ns);
    if (next_sample_sequence_ - oldest_sample_sequence_ >= time_samples_.size()) {
      ++window_capacity_errors_;
      return FeatureUpdateResult::TimeWindowCapacityExceeded;
    }

    const auto trade_flow = pending_trade_flow_.as_int64();
    trade_flow_100ms_.add(trade_flow);
    trade_flow_1s_.add(trade_flow);
    FeatureVector output{};
    if (bid && ask) {
      const auto spread = ask->price - bid->price;
      output.spread_ticks = spread / config_.price_tick;
      std::uint64_t bid_qty = 0;
      std::uint64_t ask_qty = 0;
      auto count = std::size_t{0};
      book.visit_bids([&](Price, Qty qty) noexcept {
        if (count == kFeatureTopK) return false;
        if (qty >= 0) {
          bid_qty = feature_detail::saturating_qty_add(bid_qty, static_cast<std::uint64_t>(qty));
        }
        ++count;
        return count < kFeatureTopK;
      });
      count = 0;
      book.visit_asks([&](Price, Qty qty) noexcept {
        if (count == kFeatureTopK) return false;
        if (qty >= 0) {
          ask_qty = feature_detail::saturating_qty_add(ask_qty, static_cast<std::uint64_t>(qty));
        }
        ++count;
        return count < kFeatureTopK;
      });

      const auto l1_ratio = imbalance_q20(static_cast<std::uint64_t>(std::max<Qty>(0, bid->qty)),
                                          static_cast<std::uint64_t>(std::max<Qty>(0, ask->qty)));
      const auto k_ratio = imbalance_q20(bid_qty, ask_qty);
      output.imbalance_l1_q20 = static_cast<std::int32_t>(l1_ratio);
      output.imbalance_k_q20 = static_cast<std::int32_t>(k_ratio);
      output.microprice_offset_q20_ticks =
          feature_detail::multiply_q20_half(output.spread_ticks, l1_ratio);
    }

    const auto ofi = have_previous_best_ ? order_flow_imbalance(bid, ask) : 0;
    update_ofi_windows(ofi, output);
    output.trade_flow_100ms = trade_flow_100ms_.as_int64();
    output.trade_flow_1s = trade_flow_1s_.as_int64();
    if (has_mid && cursor_100ms_sequence_ < next_sample_sequence_) {
      const auto index100 = static_cast<std::size_t>(cursor_100ms_sequence_ & sample_mask());
      const auto& old100 = time_samples_[index100];
      if (old100.has_mid) {
        output.mid_return_100ms_q20_ticks = feature_detail::signed_price_return_q20(
            {mid2, old100.mid2_price, static_cast<std::uint64_t>(config_.price_tick)});
      }
    }
    if (has_mid && oldest_sample_sequence_ < next_sample_sequence_) {
      const auto index1s = static_cast<std::size_t>(oldest_sample_sequence_ & sample_mask());
      const auto& old1s = time_samples_[index1s];
      if (old1s.has_mid) {
        output.mid_return_1s_q20_ticks = feature_detail::signed_price_return_q20(
            {mid2, old1s.mid2_price, static_cast<std::uint64_t>(config_.price_tick)});
      }
    }

    const auto sample_index = static_cast<std::size_t>(next_sample_sequence_ & sample_mask());
    time_samples_[sample_index] = TimeSample{event.ts_ns, mid2, trade_flow, has_mid};
    ++next_sample_sequence_;
    ++message_count_;

    previous_bid_ = bid;
    previous_ask_ = ask;
    have_previous_best_ = true;
    latest_ = FeatureSnapshot{event.ts_ns, mid2, has_mid, output};
    pending_trade_flow_ = {};
    return FeatureUpdateResult::Updated;
  }

  void reset() noexcept {
    oldest_sample_sequence_ = 0;
    cursor_100ms_sequence_ = 0;
    next_sample_sequence_ = 0;
    message_count_ = 0;
    trade_flow_100ms_ = {};
    trade_flow_1s_ = {};
    pending_trade_flow_ = {};
    ofi_ring_.fill(0);
    ofi_sums_ = {};
    previous_bid_.reset();
    previous_ask_.reset();
    have_previous_best_ = false;
    have_event_time_ = false;
    last_event_ts_ns_ = 0;
    latest_ = {};
  }

  [[nodiscard]] const FeatureSnapshot& latest() const noexcept { return latest_; }
  [[nodiscard]] std::uint64_t window_capacity_errors() const noexcept {
    return window_capacity_errors_;
  }
  [[nodiscard]] bool valid_config() const noexcept { return valid_config_; }

private:
  struct TimeSample {
    std::int64_t ts_ns{};
    std::uint64_t mid2_price{};
    std::int64_t trade_flow{};
    bool has_mid{};
  };

  [[nodiscard]] static bool is_power_of_two(std::size_t value) noexcept {
    return value != 0 && (value & (value - 1U)) == 0;
  }

  [[nodiscard]] std::size_t sample_mask() const noexcept { return time_samples_.size() - 1U; }

  [[nodiscard]] static std::int64_t imbalance_q20(std::uint64_t bid_qty,
                                                  std::uint64_t ask_qty) noexcept {
    const auto denominator = bid_qty + ask_qty;
    if (denominator == 0) return 0;
    const bool negative = bid_qty < ask_qty;
    const auto numerator = negative ? ask_qty - bid_qty : bid_qty - ask_qty;
    return feature_detail::signed_fraction_q20(numerator, denominator, negative);
  }

  void advance_time_windows(std::int64_t ts_ns) noexcept {
    const auto cutoff100 = ts_ns > kFeatureWindow100MsNs ? ts_ns - kFeatureWindow100MsNs : 0;
    const auto cutoff1s = ts_ns > kFeatureWindow1SecNs ? ts_ns - kFeatureWindow1SecNs : 0;
    while (cursor_100ms_sequence_ < next_sample_sequence_) {
      const auto index = static_cast<std::size_t>(cursor_100ms_sequence_ & sample_mask());
      const auto& sample = time_samples_[index];
      if (sample.ts_ns >= cutoff100) break;
      trade_flow_100ms_.subtract(sample.trade_flow);
      ++cursor_100ms_sequence_;
    }
    while (oldest_sample_sequence_ < next_sample_sequence_) {
      const auto index = static_cast<std::size_t>(oldest_sample_sequence_ & sample_mask());
      const auto& sample = time_samples_[index];
      if (sample.ts_ns >= cutoff1s) break;
      trade_flow_1s_.subtract(sample.trade_flow);
      ++oldest_sample_sequence_;
    }
  }

  [[nodiscard]] std::int64_t order_flow_imbalance(std::optional<Level> bid,
                                                  std::optional<Level> ask) const noexcept {
    feature_detail::WideSigned total;
    if (previous_bid_ && bid) {
      if (bid->price >= previous_bid_->price) total.add(bid->qty);
      if (bid->price <= previous_bid_->price) total.subtract(previous_bid_->qty);
    } else if (previous_bid_) {
      total.subtract(previous_bid_->qty);
    } else if (bid) {
      total.add(bid->qty);
    }
    if (previous_ask_ && ask) {
      if (ask->price <= previous_ask_->price) total.subtract(ask->qty);
      if (ask->price >= previous_ask_->price) total.add(previous_ask_->qty);
    } else if (previous_ask_) {
      total.add(previous_ask_->qty);
    } else if (ask) {
      total.subtract(ask->qty);
    }
    return total.as_int64();
  }

  void update_ofi_windows(std::int64_t value, FeatureVector& output) noexcept {
    const auto index = static_cast<std::size_t>(message_count_ % kFeatureMaxOfiWindow);
    const auto update_window = [&](std::size_t window, feature_detail::WideSigned& sum) noexcept {
      if (message_count_ >= window) {
        const auto old_index = static_cast<std::size_t>(
            (message_count_ + kFeatureMaxOfiWindow - window) % kFeatureMaxOfiWindow);
        sum.subtract(ofi_ring_[old_index]);
      }
      sum.add(value);
      return sum.as_int64();
    };
    output.ofi_10 = update_window(10, ofi_sums_[0]);
    output.ofi_50 = update_window(50, ofi_sums_[1]);
    output.ofi_200 = update_window(200, ofi_sums_[2]);
    ofi_ring_[index] = value;
  }

  FeatureEngineConfig config_{};
  std::vector<TimeSample> time_samples_;
  bool valid_config_{};
  std::uint64_t oldest_sample_sequence_{};
  std::uint64_t cursor_100ms_sequence_{};
  std::uint64_t next_sample_sequence_{};
  std::uint64_t message_count_{};
  std::uint64_t window_capacity_errors_{};
  feature_detail::WideSigned trade_flow_100ms_{};
  feature_detail::WideSigned trade_flow_1s_{};
  feature_detail::WideSigned pending_trade_flow_{};
  std::array<std::int64_t, kFeatureMaxOfiWindow> ofi_ring_{};
  std::array<feature_detail::WideSigned, 3> ofi_sums_{};
  std::optional<Level> previous_bid_{};
  std::optional<Level> previous_ask_{};
  bool have_previous_best_{};
  bool have_event_time_{};
  std::int64_t last_event_ts_ns_{};
  FeatureSnapshot latest_{};
};

} // namespace tickbook
