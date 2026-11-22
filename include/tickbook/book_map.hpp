#pragma once
#include <cstddef>
#include <functional>
#include <map>
#include <optional>

#include "tickbook/types.hpp"

namespace tickbook {

struct Level {
  Price price;
  Qty qty;
};

// Baseline L2 (price-level) order book on std::map. Correct and simple; slow because
// of node allocations. Later layouts (flat vector, offset array) must expose the same
// interface so the benchmark can compare them.
class BookMap {
public:
  void clear() {
    bids_.clear();
    asks_.clear();
  }

  // Apply one level change. qty == 0 removes the level.
  void apply(const Event& e) {
    if (e.type == EventType::Trade) return;
    if (e.qty < 0) return; // Negative absolute quantities are invalid level updates.
    if (e.side == Side::Bid) update(bids_, e);
    else update(asks_, e);
  }

  [[nodiscard]] std::optional<Level> best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return Level{bids_.begin()->first, bids_.begin()->second};
  }
  [[nodiscard]] std::optional<Level> best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return Level{asks_.begin()->first, asks_.begin()->second};
  }
  [[nodiscard]] std::size_t bid_levels() const { return bids_.size(); }
  [[nodiscard]] std::size_t ask_levels() const { return asks_.size(); }

  // Visit levels best-first without building a temporary container.
  template <typename Visitor>
  void visit_bids(Visitor&& visitor) const noexcept(noexcept(visitor(Price{}, Qty{}))) {
    for (const auto& [price, qty] : bids_) visitor(price, qty);
  }

  template <typename Visitor>
  void visit_asks(Visitor&& visitor) const noexcept(noexcept(visitor(Price{}, Qty{}))) {
    for (const auto& [price, qty] : asks_) visitor(price, qty);
  }

  // True if the book is crossed (best bid >= best ask): a sign of a sync error.
  [[nodiscard]] bool crossed() const {
    const auto b = best_bid();
    const auto a = best_ask();
    return b && a && b->price >= a->price;
  }

private:
  template <typename Map> static void update(Map& m, const Event& e) {
    if (e.qty == 0) m.erase(e.price);
    else m[e.price] = e.qty;
  }

  std::map<Price, Qty, std::greater<Price>> bids_; // best bid first
  std::map<Price, Qty, std::less<Price>> asks_;    // best ask first
};

} // namespace tickbook
