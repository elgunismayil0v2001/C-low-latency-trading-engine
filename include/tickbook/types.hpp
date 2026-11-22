#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace tickbook {

// Prices and quantities are fixed-point integers. Never use double on the hot path.
// Example: with price_scale = 2, the price 43000.12 is stored as 4300012.
using Price = std::int64_t;
using Qty = std::int64_t;

enum class Side : std::uint8_t { Bid = 0, Ask = 1 };
enum class EventType : std::uint8_t { BookUpdate = 0, Trade = 1, SnapshotLevel = 2 };

inline constexpr std::uint8_t kEventFlagEndOfMessage = 1U << 0U; // Last level in a message.
inline constexpr std::uint8_t kEventFlagSnapshotStart = 1U
                                                        << 1U; // Clear book before this snapshot.

// One flattened price-level change (or trade). A Binance depth message with
// several levels becomes several Events that share first_id / last_id.
struct Event {
  std::int64_t ts_ns;     // receive time (live) or recorded time (replay)
  std::uint64_t first_id; // Binance "U"
  std::uint64_t last_id;  // Binance "u"
  Price price;
  Qty qty; // 0 on a BookUpdate means "remove this level"
  EventType type;
  Side side;
  std::uint8_t flags{0};
  std::uint8_t padding[5]{}; // Serialized as zeros for deterministic binary files.
};

static_assert(std::is_standard_layout_v<Event>);
static_assert(std::is_trivially_copyable_v<Event>);
static_assert(alignof(Event) == 8);
static_assert(offsetof(Event, type) == 40);
static_assert(offsetof(Event, side) == 41);
static_assert(offsetof(Event, flags) == 42);
static_assert(offsetof(Event, padding) == 43);
static_assert(sizeof(Event) == 48, "Event layout changed: update the binary file format");

} // namespace tickbook
