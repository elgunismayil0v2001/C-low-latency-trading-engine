#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "tickbook/binary_replay.hpp"
#include "tickbook/book_map.hpp"
#include "tickbook/books/flat_book.hpp"
#include "tickbook/books/offset_book.hpp"
#include "tickbook/books/order_book.hpp"

namespace {

using namespace tickbook;

constexpr std::size_t kTopK = 5;
constexpr std::uint64_t kMaximumWindow = 4'000'000;

struct TopK {
  std::array<Level, kTopK> levels{};
  std::size_t count{};

  void operator()(Price price, Qty qty) noexcept {
    if (count < levels.size()) levels[count++] = Level{price, qty};
  }
};

template <OrderBook Left, OrderBook Right>
[[nodiscard]] bool same_book(const Left& left, const Right& right) noexcept {
  if (left.best_bid() != right.best_bid() || left.best_ask() != right.best_ask() ||
      left.bid_levels() != right.bid_levels() || left.ask_levels() != right.ask_levels() ||
      left.crossed() != right.crossed()) {
    return false;
  }

  TopK left_bids;
  TopK right_bids;
  TopK left_asks;
  TopK right_asks;
  left.visit_bids([&left_bids](Price price, Qty qty) noexcept { left_bids(price, qty); });
  right.visit_bids([&right_bids](Price price, Qty qty) noexcept { right_bids(price, qty); });
  left.visit_asks([&left_asks](Price price, Qty qty) noexcept { left_asks(price, qty); });
  right.visit_asks([&right_asks](Price price, Qty qty) noexcept { right_asks(price, qty); });
  if (left_bids.count != right_bids.count || left_asks.count != right_asks.count) return false;
  return std::equal(left_bids.levels.begin(), left_bids.levels.begin() + left_bids.count,
                    right_bids.levels.begin()) &&
         std::equal(left_asks.levels.begin(), left_asks.levels.begin() + left_asks.count,
                    right_asks.levels.begin());
}

void usage(std::ostream& output) {
  output << "Usage: compare_books FILE.bin [--price-tick FIXED_POINT_UNITS]\n";
}

[[nodiscard]] bool parse_price_tick(std::string_view text, Price& price_tick) noexcept {
  if (text.empty()) return false;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), price_tick);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && price_tick > 0;
}

} // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    usage(std::cout);
    return 0;
  }
  if (argc != 2 && argc != 4) {
    usage(std::cerr);
    return 2;
  }
  Price price_tick = 1;
  if (argc == 4 &&
      (std::string_view(argv[2]) != "--price-tick" || !parse_price_tick(argv[3], price_tick))) {
    std::cerr << "compare_books: price tick must be a positive fixed-point value\n";
    return 2;
  }

  BinaryReplay replay;
  std::string error;
  if (!replay.open(argv[1], error)) {
    std::cerr << "compare_books: " << error << '\n';
    return 1;
  }

  Price minimum_price = std::numeric_limits<Price>::max();
  Price maximum_price = std::numeric_limits<Price>::min();
  while (const auto event_view = replay.next()) {
    if (event_view->type() == EventType::Trade || event_view->qty() < 0) continue;
    if (event_view->price() % price_tick != 0) {
      std::cerr << "compare_books: event price is not aligned to --price-tick\n";
      return 1;
    }
    minimum_price = std::min(minimum_price, event_view->price());
    maximum_price = std::max(maximum_price, event_view->price());
  }

  std::uint64_t window_size = 1;
  if (minimum_price <= maximum_price) {
    const auto distance =
        static_cast<std::uint64_t>(maximum_price) - static_cast<std::uint64_t>(minimum_price);
    const auto ticks = distance / static_cast<std::uint64_t>(price_tick);
    if (ticks >= kMaximumWindow) {
      std::cerr << "compare_books: observed price span exceeds the 4000000-tick safety limit\n";
      return 1;
    }
    window_size = ticks + 1;
  }

  const auto capacity = static_cast<std::size_t>(window_size);
  BookMap baseline;
  FlatBook flat(capacity);
  OffsetArrayBook offset(capacity, price_tick);
  replay.rewind();

  std::uint64_t event_count = 0;
  while (const auto event_view = replay.next()) {
    const auto event = event_view->copy();
    if ((event.flags & kEventFlagSnapshotStart) != 0) {
      baseline.clear();
      flat.clear();
      offset.clear();
    }
    baseline.apply(event);
    flat.apply(event);
    offset.apply(event);
    if (!same_book(baseline, flat) || !same_book(baseline, offset)) {
      std::cerr << "compare_books: mismatch after event " << event_count << '\n';
      return 1;
    }
    ++event_count;
  }

  std::cout << "events=" << event_count << '\n'
            << "price_window_ticks=" << window_size << '\n'
            << "price_tick_fixed_point_units=" << price_tick << '\n'
            << "top_k_checked=" << kTopK << '\n'
            << "differential_result=match\n";
  return 0;
}
