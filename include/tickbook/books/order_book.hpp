#pragma once

#include <concepts>
#include <cstddef>
#include <optional>

#include "tickbook/types.hpp"

namespace tickbook {

struct Level {
  Price price;
  Qty qty;

  [[nodiscard]] bool operator==(const Level&) const noexcept = default;
};

template <typename Book>
concept OrderBook = requires(Book& mutable_book, const Book& book, const Event& event) {
  { mutable_book.clear() } -> std::same_as<void>;
  { mutable_book.apply(event) } -> std::same_as<void>;
  { book.best_bid() } -> std::same_as<std::optional<Level>>;
  { book.best_ask() } -> std::same_as<std::optional<Level>>;
  { book.bid_levels() } -> std::same_as<std::size_t>;
  { book.ask_levels() } -> std::same_as<std::size_t>;
  { book.crossed() } -> std::same_as<bool>;
  book.visit_bids([](Price, Qty) noexcept {});
  book.visit_asks([](Price, Qty) noexcept {});
};

} // namespace tickbook
