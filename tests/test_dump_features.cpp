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

#include "dump_features.hpp"
#include "tickbook/binary_file.hpp"
#include "tickbook/features/feature_dump.hpp"
#include "tickbook/types.hpp"

namespace {

class TempFiles {
public:
  TempFiles() {
    static std::atomic<std::uint64_t> sequence{};
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ = std::filesystem::temp_directory_path() /
                 ("tickbook_dump_features_" + std::to_string(nonce) + "_" +
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

void write_u64(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    bytes[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xffU);
  }
}

std::array<std::byte, tickbook::kBinaryEventSize> encode_event(const tickbook::Event& event) {
  std::array<std::byte, tickbook::kBinaryEventSize> bytes{};
  write_u64(bytes, 0, std::bit_cast<std::uint64_t>(event.ts_ns));
  write_u64(bytes, 8, event.first_id);
  write_u64(bytes, 16, event.last_id);
  write_u64(bytes, 24, std::bit_cast<std::uint64_t>(event.price));
  write_u64(bytes, 32, std::bit_cast<std::uint64_t>(event.qty));
  bytes[40] = static_cast<std::byte>(event.type);
  bytes[41] = static_cast<std::byte>(event.side);
  bytes[42] = static_cast<std::byte>(event.flags);
  return bytes;
}

tickbook::Event book_event(std::int64_t ts_ns, tickbook::Side side, tickbook::Price price,
                           tickbook::Qty qty, std::uint8_t flags) {
  tickbook::Event event{};
  event.ts_ns = ts_ns;
  event.price = price;
  event.qty = qty;
  event.side = side;
  event.type = tickbook::EventType::BookUpdate;
  event.flags = flags;
  return event;
}

bool write_replay(const std::filesystem::path& path, std::span<const tickbook::Event> events) {
  std::array<char, 16> symbol{};
  constexpr std::string_view name = "BTCUSDT";
  std::copy(name.begin(), name.end(), symbol.begin());
  const auto header = tickbook::make_binary_file_header(2, 8, symbol, events.size());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(header.data()),
               static_cast<std::streamsize>(header.size()));
  for (const auto& event : events) {
    const auto bytes = encode_event(event);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  return output.good();
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  const std::vector<char> chars{std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>()};
  std::vector<std::byte> bytes(chars.size());
  for (std::size_t i = 0; i < chars.size(); ++i) {
    bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(chars[i]));
  }
  return bytes;
}

} // namespace

TEST(DumpFeatures, SamplesCausallyLabelsFirstFutureSampleAndIsDeterministic) {
  TempFiles files;
  std::vector<tickbook::Event> events;
  events.push_back(book_event(0, tickbook::Side::Bid, 100, 3, tickbook::kEventFlagSnapshotStart));
  auto ask = book_event(0, tickbook::Side::Ask, 101, 1, tickbook::kEventFlagEndOfMessage);
  ask.type = tickbook::EventType::SnapshotLevel;
  events[0].type = tickbook::EventType::SnapshotLevel;
  events.push_back(ask);
  for (std::int64_t second = 1; second <= 11; ++second) {
    events.push_back(book_event(second * 1'000'000'000LL, tickbook::Side::Bid, 100 + second, 3, 0));
    events.push_back(book_event(second * 1'000'000'000LL, tickbook::Side::Ask, 100 + second, 0, 0));
    events.push_back(book_event(second * 1'000'000'000LL, tickbook::Side::Ask, 101 + second, 1,
                                tickbook::kEventFlagEndOfMessage));
  }
  ASSERT_TRUE(write_replay(files.file("input.bin"), events));

  tickbook::feature_dump_tool::Options options{};
  options.input_path = files.file("input.bin");
  options.output_path = files.file("first.tbf");
  options.sample_interval_ns = 1'000'000'000ULL;
  const auto first = tickbook::feature_dump_tool::dump_feature_file(options);
  ASSERT_TRUE(first.success) << first.error;
  EXPECT_EQ(first.event_count, events.size());
  EXPECT_EQ(first.message_count, 12U);
  EXPECT_EQ(first.sample_count, 2U);

  options.output_path = files.file("second.tbf");
  const auto second = tickbook::feature_dump_tool::dump_feature_file(options);
  ASSERT_TRUE(second.success) << second.error;
  EXPECT_EQ(read_file(files.file("first.tbf")), read_file(files.file("second.tbf")));

  const auto bytes = read_file(files.file("first.tbf"));
  tickbook::FeatureDumpHeader header{};
  ASSERT_EQ(tickbook::validate_feature_dump_header(bytes, bytes.size(), header),
            tickbook::FeatureDumpError::None);
  EXPECT_EQ(header.sample_count, 2U);
  for (std::size_t row = 0; row < header.sample_count; ++row) {
    const auto offset = tickbook::kFeatureDumpHeaderSize + row * tickbook::kFeatureDumpRecordSize;
    const auto record = tickbook::decode_feature_dump_record(
        std::span<const std::byte, tickbook::kFeatureDumpRecordSize>(
            bytes.data() + offset, tickbook::kFeatureDumpRecordSize));
    EXPECT_EQ(record.ts_ns, static_cast<std::int64_t>(row) * 1'000'000'000LL);
    EXPECT_EQ(record.forward_mid_return_q20_ticks[0], tickbook::kFeatureScale);
    EXPECT_EQ(record.forward_mid_return_q20_ticks[1], 3 * tickbook::kFeatureScale);
    EXPECT_EQ(record.forward_mid_return_q20_ticks[2], 10 * tickbook::kFeatureScale);
  }
}
