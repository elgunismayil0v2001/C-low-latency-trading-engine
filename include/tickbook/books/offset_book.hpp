#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "tickbook/books/order_book.hpp"

namespace tickbook {

// Fixed price window indexed directly by integer price. When an update falls outside
// the window, the window slides just far enough to include it and drops levels shifted
// beyond the opposite edge. Capacity is fixed at construction; no update allocates.
class OffsetArrayBook {
public:
  explicit OffsetArrayBook(std::size_t window_size = 65'536, Price price_tick = 1)
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
    origin_ = 0;
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
    ensure_window(event.price);
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
      if (bids_[slot] != 0) visitor(price_at(i), bids_[slot]);
    }
  }

  template <typename Visitor>
  void visit_asks(Visitor&& visitor) const noexcept(noexcept(visitor(Price{}, Qty{}))) {
    if (best_ask_index_ == empty_index()) return;
    for (std::size_t i = best_ask_index_; i < asks_.size(); ++i) {
      const auto slot = physical_index(i);
      if (asks_[slot] != 0) visitor(price_at(i), asks_[slot]);
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
    return physical_index(logical_index, origin_, bids_.size());
  }

  [[nodiscard]] static std::size_t physical_index(std::size_t logical_index, std::size_t origin,
                                                  std::size_t window_size) noexcept {
    const auto until_end = window_size - origin;
    return logical_index < until_end ? origin + logical_index : logical_index - until_end;
  }

  [[nodiscard]] static std::uint64_t distance(Price high, Price low) noexcept {
    return static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low);
  }

  [[nodiscard]] Price initial_base(Price price) const noexcept {
    const auto highest_base = std::numeric_limits<Price>::max() - window_span_;
    return price > highest_base ? price - window_span_ : price;
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

  void ensure_window(Price price) noexcept {
    if (!initialized_) {
      initialized_ = true;
      base_price_ = initial_base(price);
      return;
    }

    if (price < base_price_) {
      const auto delta = distance(base_price_, price) / static_cast<std::uint64_t>(price_tick_);
      if (delta >= bids_.size()) {
        reset_levels();
      } else {
        shift_window_lower(static_cast<std::size_t>(delta));
      }
      base_price_ = price;
      return;
    }

    const auto price_offset =
        distance(price, base_price_) / static_cast<std::uint64_t>(price_tick_);
    if (price_offset < bids_.size()) return;

    const auto delta = price_offset - (bids_.size() - 1);
    if (delta >= bids_.size()) reset_levels();
    else shift_window_higher(static_cast<std::size_t>(delta));
    base_price_ = price - window_span_;
  }

  void shift_window_lower(std::size_t delta) noexcept {
    const auto size = bids_.size();
    const auto next_origin = origin_ >= delta ? origin_ - delta : size - (delta - origin_);
    clear_expired(bids_, bid_count_, next_origin, 0, delta);
    clear_expired(asks_, ask_count_, next_origin, 0, delta);
    origin_ = next_origin;
    shift_best_after_lower(bids_, best_bid_index_, delta, true);
    shift_best_after_lower(asks_, best_ask_index_, delta, false);
  }

  void shift_window_higher(std::size_t delta) noexcept {
    const auto size = bids_.size();
    const auto until_end = size - origin_;
    const auto next_origin = delta >= until_end ? delta - until_end : origin_ + delta;
    const auto first_expired = size - delta;
    clear_expired(bids_, bid_count_, next_origin, first_expired, size);
    clear_expired(asks_, ask_count_, next_origin, first_expired, size);
    origin_ = next_origin;
    shift_best_after_higher(bids_, best_bid_index_, delta, true);
    shift_best_after_higher(asks_, best_ask_index_, delta, false);
  }

  void clear_expired(std::vector<Qty>& levels, std::size_t& count, std::size_t new_origin,
                     std::size_t first, std::size_t last) noexcept {
    for (std::size_t i = first; i < last; ++i) {
      const auto slot = physical_index(i, new_origin, levels.size());
      if (levels[slot] != 0) {
        levels[slot] = 0;
        --count;
      }
    }
  }

  void shift_best_after_lower(const std::vector<Qty>& levels, std::size_t& best_index,
                              std::size_t delta, bool bids) const noexcept {
    if (best_index == empty_index()) return;
    if (best_index < levels.size() - delta) {
      best_index += delta;
      return;
    }
    best_index = find_best(levels, bids);
  }

  void shift_best_after_higher(const std::vector<Qty>& levels, std::size_t& best_index,
                               std::size_t delta, bool bids) const noexcept {
    if (best_index == empty_index()) return;
    if (best_index >= delta) {
      best_index -= delta;
      return;
    }
    best_index = bids ? empty_index() : find_best(levels, false);
  }

  [[nodiscard]] std::size_t find_best(const std::vector<Qty>& levels, bool bids) const noexcept {
    if (bids) {
      for (std::size_t i = levels.size(); i > 0;) {
        --i;
        if (levels[physical_index(i)] != 0) return i;
      }
    } else {
      for (std::size_t i = 0; i < levels.size(); ++i) {
        if (levels[physical_index(i)] != 0) return i;
      }
    }
    return empty_index();
  }

  void reset_levels() noexcept {
    std::fill(bids_.begin(), bids_.end(), Qty{0});
    std::fill(asks_.begin(), asks_.end(), Qty{0});
    bid_count_ = 0;
    ask_count_ = 0;
    origin_ = 0;
    best_bid_index_ = empty_index();
    best_ask_index_ = empty_index();
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
  // Rebase the logical price window by rotating its origin instead of moving all levels.
  std::size_t origin_{};
  bool initialized_{};
  bool valid_{};
};

} // namespace tickbook
