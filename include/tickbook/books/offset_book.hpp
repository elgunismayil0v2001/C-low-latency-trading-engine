#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "tickbook/books/order_book.hpp"

namespace tickbook {

inline constexpr std::size_t kDefaultOffsetArrayWindowSize = 65'536;

// Fixed price window indexed directly by integer price. The first accepted price centers
// the window; all later out-of-window levels are ignored so a deep update cannot move the
// window or evict retained levels. Capacity is fixed at construction; no update allocates.
class OffsetArrayBook {
public:
  explicit OffsetArrayBook(std::size_t window_size = kDefaultOffsetArrayWindowSize,
                           Price price_tick = 1)
      : bids_(window_size, 0), asks_(window_size, 0), price_tick_(price_tick),
        best_bid_index_(window_size), best_ask_index_(window_size) {
    if (price_tick_ <= 0) return;
    const auto max_span =
        static_cast<std::uint64_t>(std::numeric_limits<Price>::max() / price_tick_);
    const auto requested_span = static_cast<std::uint64_t>(window_size == 0 ? 0 : window_size - 1);
    if (requested_span > max_span) return;
    window_span_ = static_cast<Price>(requested_span * static_cast<std::uint64_t>(price_tick_));
    valid_ = true;
  }

  void clear() noexcept {
    std::fill(bids_.begin(), bids_.end(), Qty{0});
    std::fill(asks_.begin(), asks_.end(), Qty{0});
    initialized_ = false;
    bid_count_ = 0;
    ask_count_ = 0;
    best_bid_index_ = empty_index();
    best_ask_index_ = empty_index();
  }

  void apply(const Event& event) noexcept {
    if (event.type == EventType::Trade || event.qty < 0 || bids_.empty() || !valid_ ||
        event.price % price_tick_ != 0) {
      return;
    }
    if (!ensure_window(event.price)) return;
    std::size_t index = 0;
    if (!offset(event.price, index)) return;
    if (event.side == Side::Bid) update(bids_, bid_count_, best_bid_index_, index, event.qty, true);
    else update(asks_, ask_count_, best_ask_index_, index, event.qty, false);
  }

  [[nodiscard]] std::optional<Level> best_bid() const noexcept {
    if (best_bid_index_ == empty_index()) return std::nullopt;
    return Level{price_at(best_bid_index_), bids_[physical_index(best_bid_index_)]};
  }
  [[nodiscard]] std::optional<Level> best_ask() const noexcept {
    if (best_ask_index_ == empty_index()) return std::nullopt;
    return Level{price_at(best_ask_index_), asks_[physical_index(best_ask_index_)]};
  }
  [[nodiscard]] std::size_t bid_levels() const noexcept { return bid_count_; }
  [[nodiscard]] std::size_t ask_levels() const noexcept { return ask_count_; }

  template <typename Visitor>
  void visit_bids(Visitor&& visitor) const noexcept(noexcept(visitor(Price{}, Qty{}))) {
    if (best_bid_index_ == empty_index()) return;
    for (std::size_t i = best_bid_index_ + 1; i > 0;) {
      --i;
      const auto slot = physical_index(i);
      if (bids_[slot] != 0 && !visit_level(visitor, price_at(i), bids_[slot])) return;
    }
  }

  template <typename Visitor>
  void visit_asks(Visitor&& visitor) const noexcept(noexcept(visitor(Price{}, Qty{}))) {
    if (best_ask_index_ == empty_index()) return;
    for (std::size_t i = best_ask_index_; i < asks_.size(); ++i) {
      const auto slot = physical_index(i);
      if (asks_[slot] != 0 && !visit_level(visitor, price_at(i), asks_[slot])) return;
    }
  }

  [[nodiscard]] bool crossed() const noexcept {
    const auto bid = best_bid();
    const auto ask = best_ask();
    return bid && ask && bid->price >= ask->price;
  }

  [[nodiscard]] std::size_t window_size() const noexcept { return bids_.size(); }
  [[nodiscard]] std::optional<Price> window_base() const noexcept {
    if (!initialized_) return std::nullopt;
    return base_price_;
  }

private:
  [[nodiscard]] std::size_t empty_index() const noexcept { return bids_.size(); }

  [[nodiscard]] std::size_t physical_index(std::size_t logical_index) const noexcept {
    return logical_index;
  }

  [[nodiscard]] static std::uint64_t distance(Price high, Price low) noexcept {
    return static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low);
  }

  [[nodiscard]] Price initial_base(Price price) const noexcept {
    const auto half_ticks = bids_.size() / 2;
    const auto half_span = static_cast<Price>(
        static_cast<std::uint64_t>(half_ticks) * static_cast<std::uint64_t>(price_tick_));
    const auto upper_padding = window_span_ - half_span;
    const auto minimum = std::numeric_limits<Price>::min();
    const auto maximum = std::numeric_limits<Price>::max();
    if (price < minimum + half_span) return price;
    if (price > maximum - upper_padding) return price - window_span_;
    return price - half_span;
  }

  [[nodiscard]] bool offset(Price price, std::size_t& index) const noexcept {
    if (price < base_price_) return false;
    const auto delta = distance(price, base_price_);
    const auto tick = static_cast<std::uint64_t>(price_tick_);
    if (delta % tick != 0) return false;
    const auto tick_offset = delta / tick;
    if (tick_offset >= bids_.size()) return false;
    index = static_cast<std::size_t>(tick_offset);
    return true;
  }

  [[nodiscard]] bool ensure_window(Price price) noexcept {
    if (!initialized_) {
      initialized_ = true;
      base_price_ = initial_base(price);
      return true;
    }
    std::size_t ignored_index = 0;
    return offset(price, ignored_index);
  }

  void update(std::vector<Qty>& levels, std::size_t& count, std::size_t& best_index,
              std::size_t index, Qty qty, bool bids) noexcept {
    const auto slot = physical_index(index);
    const Qty previous = levels[slot];
    if (qty == 0) {
      if (previous == 0) return;
      levels[slot] = 0;
      --count;
      if (index == best_index) {
        best_index = empty_index();
        if (bids) {
          for (std::size_t i = index; i > 0;) {
            --i;
            if (levels[physical_index(i)] != 0) {
              best_index = i;
              break;
            }
          }
        } else {
          for (std::size_t i = index + 1; i < levels.size(); ++i) {
            if (levels[physical_index(i)] != 0) {
              best_index = i;
              break;
            }
          }
        }
      }
      return;
    }

    levels[slot] = qty;
    if (previous == 0) ++count;
    if (best_index == empty_sentinel(levels.size()) || (bids && index > best_index) ||
        (!bids && index < best_index)) {
      best_index = index;
    }
  }

  [[nodiscard]] Price price_at(std::size_t index) const noexcept {
    return base_price_ + static_cast<Price>(index) * price_tick_;
  }

  [[nodiscard]] static std::size_t empty_sentinel(std::size_t window_size) noexcept {
    return window_size;
  }

  std::vector<Qty> bids_;
  std::vector<Qty> asks_;
  Price base_price_{};
  Price price_tick_{1};
  Price window_span_{};
  std::size_t bid_count_{};
  std::size_t ask_count_{};
  std::size_t best_bid_index_{};
  std::size_t best_ask_index_{};
  bool initialized_{};
  bool valid_{};
};

} // namespace tickbook
