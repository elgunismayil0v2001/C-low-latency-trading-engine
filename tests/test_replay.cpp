#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "tickbook/binary_file.hpp"
#include "tickbook/replay.hpp"
#include "tickbook/types.hpp"

namespace {

class TempFiles {
public:
  TempFiles() {
    static std::atomic<std::uint64_t> sequence{};
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ = std::filesystem::temp_directory_path() /
                 ("tickbook_replay_" + std::to_string(nonce) + "_" +
                  std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(directory_);
  }

  ~TempFiles() {
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }

  [[nodiscard]] std::filesystem::path file(std::string_view name) const {
    return directory_ / name;
  }

private:
  std::filesystem::path directory_;
};

void write_u64_le(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    bytes[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xffU);
  }
}

std::array<std::byte, tickbook::kBinaryEventSize> encode_event(const tickbook::Event& event) {
  std::array<std::byte, tickbook::kBinaryEventSize> bytes{};
  write_u64_le(bytes, 0, std::bit_cast<std::uint64_t>(event.ts_ns));
  write_u64_le(bytes, 8, event.first_id);
  write_u64_le(bytes, 16, event.last_id);
  write_u64_le(bytes, 24, std::bit_cast<std::uint64_t>(event.price));
  write_u64_le(bytes, 32, std::bit_cast<std::uint64_t>(event.qty));
  bytes[40] = static_cast<std::byte>(event.type);
  bytes[41] = static_cast<std::byte>(event.side);
  bytes[42] = static_cast<std::byte>(event.flags);
  for (std::size_t i = 0; i < std::size(event.padding); ++i) {
    bytes[43 + i] = static_cast<std::byte>(event.padding[i]);
  }
  return bytes;
}

std::array<char, 16> test_symbol() {
  std::array<char, 16> symbol{};
  constexpr std::string_view value = "BTCUSDT";
  std::copy(value.begin(), value.end(), symbol.begin());
  return symbol;
}

std::vector<std::byte> make_file(std::span<const tickbook::Event> events) {
  std::vector<std::byte> bytes(tickbook::kBinaryFileHeaderSize +
                               events.size() * tickbook::kBinaryEventSize);
  const auto header = tickbook::make_binary_file_header(2, 8, test_symbol(), events.size());
  std::copy(header.begin(), header.end(), bytes.begin());
  for (std::size_t i = 0; i < events.size(); ++i) {
    const auto encoded = encode_event(events[i]);
    std::copy(encoded.begin(), encoded.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(tickbook::kBinaryFileHeaderSize +
                                                          i * tickbook::kBinaryEventSize));
  }
  return bytes;
}

bool write_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return output.good();
}

tickbook::Event book_event(std::int64_t ts_ns, tickbook::Side side, tickbook::Price price,
                           tickbook::Qty qty, tickbook::EventType type,
                           std::uint8_t flags = tickbook::kEventFlagEndOfMessage) {
  tickbook::Event event{};
  event.ts_ns = ts_ns;
  event.price = price;
  event.qty = qty;
  event.type = type;
  event.side = side;
  event.flags = flags;
  return event;
}

} // namespace

TEST(Replay, GoldenFileMatchesFinalBookCountAndChecksumDeterministically) {
  TempFiles files;
  const std::array events{
      book_event(1, tickbook::Side::Bid, 100, 2, tickbook::EventType::SnapshotLevel,
                 tickbook::kEventFlagSnapshotStart),
      book_event(2, tickbook::Side::Ask, 101, 3, tickbook::EventType::SnapshotLevel),
      book_event(3, tickbook::Side::Bid, 100, 1, tickbook::EventType::BookUpdate),
      book_event(4, tickbook::Side::Bid, 99, 4, tickbook::EventType::BookUpdate),
      book_event(5, tickbook::Side::Bid, 100, 7, tickbook::EventType::Trade),
      book_event(6, tickbook::Side::Bid, 98, 5, tickbook::EventType::SnapshotLevel,
                 tickbook::kEventFlagSnapshotStart | tickbook::kEventFlagEndOfMessage),
  };
  const auto bytes = make_file(events);
  ASSERT_TRUE(write_file(files.file("golden.bin"), bytes));

  const tickbook::ReplayOptions default_options{};
  const auto first = tickbook::run_binary_replay<2>(files.file("golden.bin"), default_options);
  ASSERT_TRUE(first.success) << first.error;
  EXPECT_EQ(first.symbol, test_symbol());
  EXPECT_EQ(first.price_scale, 2);
  EXPECT_EQ(first.qty_scale, 8);
  EXPECT_EQ(first.event_count, 6u);
  EXPECT_EQ(first.bid_levels, 1u);
  EXPECT_EQ(first.ask_levels, 0u);
  ASSERT_TRUE(first.best_bid.has_value());
  EXPECT_EQ(first.best_bid->price, 98);
  EXPECT_EQ(first.best_bid->qty, 5);
  EXPECT_FALSE(first.best_ask.has_value());
  EXPECT_EQ(first.book_checksum, 17674758387495183435ULL);

  const auto second = tickbook::run_binary_replay<2>(files.file("golden.bin"));
  ASSERT_TRUE(second.success) << second.error;
  EXPECT_EQ(second.event_count, first.event_count);
  EXPECT_EQ(second.bid_levels, first.bid_levels);
  EXPECT_EQ(second.ask_levels, first.ask_levels);
  EXPECT_EQ(second.best_bid->price, first.best_bid->price);
  EXPECT_EQ(second.best_bid->qty, first.best_bid->qty);
  EXPECT_EQ(second.book_checksum, first.book_checksum);
}

TEST(Replay, RejectsInvalidBinaryInput) {
  TempFiles files;
  const std::array<tickbook::Event, 0> events{};
  auto bytes = make_file(events);
  bytes[0] = std::byte{'X'};
  ASSERT_TRUE(write_file(files.file("invalid.bin"), bytes));

  const auto result = tickbook::run_binary_replay(files.file("invalid.bin"));
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error, "binary file has the wrong magic");
}

