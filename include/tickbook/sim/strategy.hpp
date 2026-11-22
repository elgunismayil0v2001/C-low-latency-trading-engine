#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

#include "tickbook/books/order_book.hpp"
#include "tickbook/features/feature_engine.hpp"

namespace tickbook::sim {

enum class SignalKind : std::uint8_t {
  FeatureImbalance,
  NeuralDirection,
  NeuralReturn,
};

// score_q20 is an imbalance/logit score or a predicted return in Q20 ticks.
struct Signal {
  std::int64_t score_q20{};
  SignalKind kind{SignalKind::FeatureImbalance};
};

struct BookView {
  std::optional<Level> best_bid{};
  std::optional<Level> best_ask{};
  Qty position{};
  Qty projected_position{};
};

enum class OrderSide : std::uint8_t { Buy, Sell };

struct OrderIntent {
  OrderSide side{OrderSide::Buy};
  Qty qty{};
  [[nodiscard]] explicit operator bool() const noexcept { return qty > 0; }
};

class IStrategy {
public:
  virtual ~IStrategy() = default;
  [[nodiscard]] virtual OrderIntent on_signal(const Signal& signal,
                                              const BookView& book) noexcept = 0;
};

struct StrategyLimits {
  Qty max_order_qty{};
  Qty max_position_qty{};
};

namespace strategy_detail {

[[nodiscard]] inline OrderIntent target_from_score(std::int64_t score, std::int64_t threshold_q20,
                                                   StrategyLimits limits,
                                                   const BookView& book) noexcept {
  if (limits.max_order_qty <= 0 || limits.max_position_qty <= 0 || threshold_q20 < 0) return {};
  if (score > 0 && score >= threshold_q20) {
    if (book.projected_position < 0 || book.projected_position >= limits.max_position_qty)
      return {};
    return {OrderSide::Buy,
            std::min(limits.max_order_qty, limits.max_position_qty - book.projected_position)};
  }
  if (score < 0 && score <= -threshold_q20 && book.projected_position > 0) {
    return {OrderSide::Sell, std::min(limits.max_order_qty, book.projected_position)};
  }
  return {};
}

} // namespace strategy_detail

class ThresholdStrategy final : public IStrategy {
public:
  explicit ThresholdStrategy(StrategyLimits limits, std::int64_t threshold_q20)
      : limits_(limits), threshold_q20_(threshold_q20) {}

  [[nodiscard]] OrderIntent on_signal(const Signal& signal,
                                      const BookView& book) noexcept override {
    if (signal.kind != SignalKind::FeatureImbalance) return {};
    return strategy_detail::target_from_score(signal.score_q20, threshold_q20_, limits_, book);
  }

private:
  StrategyLimits limits_{};
  std::int64_t threshold_q20_{};
};

class NnStrategy final : public IStrategy {
public:
  explicit NnStrategy(StrategyLimits limits, std::int64_t return_threshold_q20,
                      std::int64_t direction_margin_q20 = 0)
      : limits_(limits), return_threshold_q20_(return_threshold_q20),
        direction_margin_q20_(direction_margin_q20) {}

  [[nodiscard]] OrderIntent on_signal(const Signal& signal,
                                      const BookView& book) noexcept override {
    if (signal.kind == SignalKind::NeuralReturn) {
      return strategy_detail::target_from_score(signal.score_q20, return_threshold_q20_, limits_,
                                                book);
    }
    if (signal.kind == SignalKind::NeuralDirection) {
      return strategy_detail::target_from_score(signal.score_q20, direction_margin_q20_, limits_,
                                                book);
    }
    return {};
  }

private:
  StrategyLimits limits_{};
  std::int64_t return_threshold_q20_{};
  std::int64_t direction_margin_q20_{};
};

[[nodiscard]] inline Signal imbalance_signal(const FeatureVector& features) noexcept {
  return {features.imbalance_l1_q20, SignalKind::FeatureImbalance};
}

} // namespace tickbook::sim
