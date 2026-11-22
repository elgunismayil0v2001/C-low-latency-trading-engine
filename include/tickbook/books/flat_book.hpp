#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

#include "tickbook/books/order_book.hpp"

namespace tickbook {

// Sorted contiguous levels with O(1) best-level queries. Each side retains at most
// capacity best levels; worse levels are ignored or evicted when a better one arrives.
// The vectors reserve their full capacity during construction and never grow in apply().
class FlatBook {
public:
  explicit FlatBook(std::size_t capacity = 4096) : capacity_(capacity) {
    bids_.reserve(capacity_);
    asks_.reserve(capacity_);
  }

  void clear() noexcept {
    bids_.clear();
    asks_.clear();
  }

  void apply(const Event& event) noexcept {
    if (event.type == EventType::Trade || event.qty < 0) return;
    if (event.side == Side::Bid) update(bids_, event.price, event.qty, true);
    else update(asks_, event.price, event.qty, false);
  }

  [[nodiscard]] std::optional<Level> best_bid() const noexcept {
    if (bids_.empty()) return std::nullopt;
    return bids_.front();
  }
  [[nodiscard]] std::optional<Level> best_ask() const noexcept {
    if (asks_.empty()) return std::nullopt;
    return asks_.front();
  }
  [[nodiscard]] std::size_t bid_levels() const noexcept { return bids_.size(); }
  [[nodiscard]] std::size_t ask_levels() const noexcept { return asks_.size(); }

  template <typename Visitor>
  void visit_bids(Visitor&& visitor) const noexcept(noexcept(visitor(Price{}, Qty{}))) {
    for (const auto& level : bids_) visitor(level.price, level.qty);
  }

  template <typename Visitor>
  void visit_asks(Visitor&& visitor) const noexcept(noexcept(visitor(Price{}, Qty{}))) {
    for (const auto& level : asks_) visitor(level.price, level.qty);
  }

  [[nodiscard]] bool crossed() const noexcept {
    const auto bid = best_bid();
    const auto ask = best_ask();
    return bid && ask && bid->price >= ask->price;
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
  [[nodiscard]] static std::size_t find(const std::vector<Level>& levels, Price price,
                                        bool bids) noexcept {
    std::size_t first = 0;
    std::size_t last = levels.size();
    while (first < last) {
      const auto middle = first + (last - first) / 2;
      const bool level_before = bids ? levels[middle].price > price : levels[middle].price < price;
      if (level_before) first = middle + 1;
      else last = middle;
    }
    return first;
  }

  void update(std::vector<Level>& levels, Price price, Qty qty, bool bids) noexcept {
    const auto index = find(levels, price, bids);
    if (index < levels.size() && levels[index].price == price) {
      if (qty != 0) {
        levels[index].qty = qty;
        return;
      }
      for (auto i = index; i + 1 < levels.size(); ++i) levels[i] = levels[i + 1];
      levels.pop_back();
      return;
    }

    if (qty == 0 || capacity_ == 0 || index >= capacity_) return;
    if (levels.size() < capacity_) {
      levels.push_back(Level{}); // reserve() in the constructor prevents a hot-path allocation.
      for (auto i = levels.size() - 1; i > index; --i) levels[i] = levels[i - 1];
      levels[index] = Level{price, qty};
      return;
    }

    for (auto i = capacity_ - 1; i > index; --i) levels[i] = levels[i - 1];
    levels[index] = Level{price, qty};
  }

  std::size_t capacity_{};
  std::vector<Level> bids_;
  std::vector<Level> asks_;
};

} // namespace tickbook
