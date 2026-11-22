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
#include <vector>

#include "tickbook/binary_file.hpp"
#include "tickbook/binary_replay.hpp"
#include "tickbook/types.hpp"

namespace {

class TempFiles {
public:
  TempFiles() {
    static std::atomic<std::uint64_t> sequence{};
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ = std::filesystem::temp_directory_path() /
                 ("tickbook_binary_replay_" + std::to_string(nonce) + "_" +
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

} // namespace

TEST(BinaryReplay, MapsValidatedEventsAsNonOwningViews) {
  TempFiles files;
  tickbook::Event first{123, 4, 5, 10001, 200000000,
                        tickbook::EventType::SnapshotLevel, tickbook::Side::Bid};
  first.flags = tickbook::kEventFlagEndOfMessage | tickbook::kEventFlagSnapshotStart;
  tickbook::Event second{456, 0, 0, 10002, 0,
                         tickbook::EventType::Trade, tickbook::Side::Ask};
  const std::array events{first, second};
  const auto bytes = make_file(events);
  ASSERT_TRUE(write_file(files.file("valid.bin"), bytes));

  tickbook::BinaryReplay replay;
  std::string error;
  ASSERT_TRUE(replay.open(files.file("valid.bin"), error)) << error;
  EXPECT_TRUE(replay.is_open());
  EXPECT_EQ(replay.event_count(), 2u);
  EXPECT_EQ(replay.price_scale(), 2);
  EXPECT_EQ(replay.qty_scale(), 8);
  EXPECT_EQ(replay.symbol(), test_symbol());
  EXPECT_EQ(replay.mapped_size(), bytes.size());

  auto first_view = replay.next();
  ASSERT_TRUE(first_view);
  EXPECT_EQ(first_view->ts_ns(), 123);
  EXPECT_EQ(first_view->first_id(), 4u);
  EXPECT_EQ(first_view->last_id(), 5u);
  EXPECT_EQ(first_view->price(), 10001);
  EXPECT_EQ(first_view->qty(), 200000000);
  EXPECT_EQ(first_view->type(), tickbook::EventType::SnapshotLevel);
  EXPECT_EQ(first_view->side(), tickbook::Side::Bid);
  EXPECT_EQ(first_view->flags(), tickbook::kEventFlagEndOfMessage |
                                     tickbook::kEventFlagSnapshotStart);
  const auto first_bytes = first_view->bytes();

  const tickbook::Event owned = first_view->copy();
  EXPECT_EQ(owned.ts_ns, first.ts_ns);
  EXPECT_EQ(owned.first_id, first.first_id);
  EXPECT_EQ(owned.last_id, first.last_id);
  EXPECT_EQ(owned.price, first.price);
  EXPECT_EQ(owned.qty, first.qty);
  EXPECT_EQ(owned.type, first.type);
  EXPECT_EQ(owned.side, first.side);
  EXPECT_EQ(owned.flags, first.flags);

  auto second_view = replay.next();
  ASSERT_TRUE(second_view);
  EXPECT_EQ(second_view->ts_ns(), 456);
  EXPECT_EQ(second_view->type(), tickbook::EventType::Trade);
  EXPECT_EQ(second_view->bytes().data(), first_bytes.data() + tickbook::kBinaryEventSize);
  EXPECT_FALSE(replay.next());

  replay.rewind();
  auto rewound_view = replay.next();
  ASSERT_TRUE(rewound_view);
  EXPECT_EQ(rewound_view->bytes().data(), first_bytes.data());
  replay.close();
  EXPECT_FALSE(replay.is_open());
  EXPECT_EQ(replay.event_count(), 0u);
}

TEST(BinaryReplay, RejectsInvalidHeaderAndCount) {
  TempFiles files;
  const std::array<tickbook::Event, 1> events{tickbook::Event{}};
  auto invalid_magic = make_file(events);
  invalid_magic[0] = std::byte{'X'};
  ASSERT_TRUE(write_file(files.file("bad-magic.bin"), invalid_magic));

  tickbook::BinaryReplay replay;
  std::string error;
  EXPECT_FALSE(replay.open(files.file("bad-magic.bin"), error));
  EXPECT_EQ(error, "binary file has the wrong magic");
  EXPECT_FALSE(replay.is_open());

  auto count_mismatch = make_file(events);
  write_u64_le(count_mismatch, 44, 2);
  ASSERT_TRUE(write_file(files.file("bad-count.bin"), count_mismatch));
  EXPECT_FALSE(replay.open(files.file("bad-count.bin"), error));
  EXPECT_EQ(error, "binary event count does not match file size");
  EXPECT_FALSE(replay.is_open());
}

TEST(BinaryReplay, AcceptsAnEmptyEventPayloadAndMissingFileFailsClearly) {
  TempFiles files;
  const std::array<tickbook::Event, 0> no_events{};
  const auto bytes = make_file(no_events);
  ASSERT_TRUE(write_file(files.file("empty.bin"), bytes));

  tickbook::BinaryReplay replay;
  std::string error;
  ASSERT_TRUE(replay.open(files.file("empty.bin"), error)) << error;
  EXPECT_EQ(replay.event_count(), 0u);
  EXPECT_FALSE(replay.next());

  EXPECT_FALSE(replay.open(files.file("missing.bin"), error));
  EXPECT_NE(error.find("cannot open binary replay"), std::string::npos);
  EXPECT_FALSE(replay.is_open());
}

