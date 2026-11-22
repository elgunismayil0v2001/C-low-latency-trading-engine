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

#include <zlib.h>

#include "tickbook/binary_file.hpp"
#include "tickbook/csv_to_bin.hpp"
#include "tickbook/types.hpp"

namespace {

class TempFiles {
public:
  TempFiles() {
    static std::atomic<std::uint64_t> sequence{};
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ = std::filesystem::temp_directory_path() /
                 ("tickbook_csv_to_bin_" + std::to_string(nonce) + "_" +
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

bool write_gzip(const std::filesystem::path& path, std::string_view contents) {
  gzFile file = gzopen(path.string().c_str(), "wb");
  if (file == nullptr) return false;
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const auto count = static_cast<unsigned int>(
        std::min<std::size_t>(contents.size() - offset, 64 * 1024));
    const int written = gzwrite(file, contents.data() + offset, count);
    if (written <= 0) {
      gzclose(file);
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return gzclose(file) == Z_OK;
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  const std::vector<char> chars{std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>()};
  std::vector<std::byte> bytes(chars.size());
  for (std::size_t i = 0; i < chars.size(); ++i) {
    bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(chars[i]));
  }
  return bytes;
}

std::uint64_t read_u64(std::span<const std::byte> bytes, std::size_t offset) {
  return tickbook::binary_file_detail::read_u64_le(bytes, offset);
}

tickbook::Event read_event(std::span<const std::byte> bytes, std::size_t index) {
  const std::size_t offset = tickbook::kBinaryFileHeaderSize +
                             index * tickbook::kBinaryEventSize;
  tickbook::Event event{};
  event.ts_ns = std::bit_cast<std::int64_t>(read_u64(bytes, offset));
  event.first_id = read_u64(bytes, offset + 8);
  event.last_id = read_u64(bytes, offset + 16);
  event.price = std::bit_cast<std::int64_t>(read_u64(bytes, offset + 24));
  event.qty = std::bit_cast<std::int64_t>(read_u64(bytes, offset + 32));
  event.type = static_cast<tickbook::EventType>(std::to_integer<std::uint8_t>(bytes[offset + 40]));
  event.side = static_cast<tickbook::Side>(std::to_integer<std::uint8_t>(bytes[offset + 41]));
  event.flags = std::to_integer<std::uint8_t>(bytes[offset + 42]);
  for (std::size_t i = 0; i < std::size(event.padding); ++i) {
    event.padding[i] = std::to_integer<std::uint8_t>(bytes[offset + 43 + i]);
  }
  return event;
}

std::uint64_t fnv1a64(std::span<const std::byte> bytes) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const std::byte byte : bytes) {
    hash ^= std::to_integer<std::uint8_t>(byte);
    hash *= 1099511628211ULL;
  }
  return hash;
}

constexpr std::string_view kBookCsv =
    "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n"
    "binance,BTCUSDT,900,900,false,bid,99.00,7.0\n"
    "binance,BTCUSDT,1000,1000,true,bid,100.00,1.0\n"
    "binance,BTCUSDT,1000,1000,true,ask,101.00,2.0\n"
    "binance,BTCUSDT,1100,1100,false,bid,100.00,3.0\n"
    "binance,BTCUSDT,1200,1200,false,ask,101.00,0.0\n"
    "binance,BTCUSDT,1300,1300,true,bid,99.00,4.0\n";

constexpr std::string_view kTradesCsv =
    "exchange,symbol,timestamp,local_timestamp,id,side,price,amount\n"
    "binance,BTCUSDT,1000,1000,1,buy,100.00,0.5\n"
    "binance,BTCUSDT,1150,1150,2,sell,100.50,0.25\n";

tickbook::CsvToBinaryConfig config_for(const TempFiles& files,
                                       const std::filesystem::path& output) {
  tickbook::CsvToBinaryConfig config;
  config.book_csv_gz = files.file("book.csv.gz");
  config.output_file = output;
  return config;
}

} // namespace

TEST(CsvToBin, MergesGzipStreamsAndMarksMessagesDeterministically) {
  TempFiles files;
  ASSERT_TRUE(write_gzip(files.file("book.csv.gz"), kBookCsv));
  ASSERT_TRUE(write_gzip(files.file("trades.csv.gz"), kTradesCsv));

  auto first_config = config_for(files, files.file("first.bin"));
  first_config.trades_csv_gz = files.file("trades.csv.gz");
  const auto first_result = tickbook::convert_tardis_csv_to_binary(first_config);
  ASSERT_TRUE(first_result.success) << first_result.error;
  EXPECT_EQ(first_result.event_count, 7u);

  const auto bytes = read_bytes(first_config.output_file);
  tickbook::BinaryFileView view;
  ASSERT_EQ(tickbook::validate_binary_file(bytes, view), tickbook::BinaryFileError::None);
  EXPECT_EQ(view.event_count, 7u);
  EXPECT_EQ(view.price_scale, 2);
  EXPECT_EQ(view.qty_scale, 8);
  EXPECT_EQ(view.symbol[0], 'B');

  const auto first = read_event(bytes, 0);
  EXPECT_EQ(first.ts_ns, 1'000'000);
  EXPECT_EQ(first.type, tickbook::EventType::SnapshotLevel);
  EXPECT_EQ(first.price, 10'000);
  EXPECT_EQ(first.qty, 100'000'000);
  EXPECT_EQ(first.first_id, 0u);
  EXPECT_EQ(first.last_id, 0u);
  EXPECT_NE(first.flags & tickbook::kEventFlagSnapshotStart, 0);
  EXPECT_EQ(first.flags & tickbook::kEventFlagEndOfMessage, 0);

  const auto second = read_event(bytes, 1);
  const auto third = read_event(bytes, 2);
  EXPECT_EQ(second.type, tickbook::EventType::SnapshotLevel);
  EXPECT_EQ(second.side, tickbook::Side::Ask);
  EXPECT_EQ(third.type, tickbook::EventType::Trade);
  EXPECT_EQ(third.side, tickbook::Side::Bid);
  EXPECT_NE(third.flags & tickbook::kEventFlagEndOfMessage, 0);

  const auto fourth = read_event(bytes, 3);
  const auto fifth = read_event(bytes, 4);
  const auto sixth = read_event(bytes, 5);
  const auto seventh = read_event(bytes, 6);
  EXPECT_EQ(fourth.ts_ns, 1'100'000);
  EXPECT_EQ(fourth.type, tickbook::EventType::BookUpdate);
  EXPECT_NE(fourth.flags & tickbook::kEventFlagEndOfMessage, 0);
  EXPECT_EQ(fifth.ts_ns, 1'150'000);
  EXPECT_EQ(fifth.type, tickbook::EventType::Trade);
  EXPECT_EQ(fifth.side, tickbook::Side::Ask);
  EXPECT_NE(fifth.flags & tickbook::kEventFlagEndOfMessage, 0);
  EXPECT_EQ(sixth.qty, 0);
  EXPECT_NE(sixth.flags & tickbook::kEventFlagEndOfMessage, 0);
  EXPECT_NE(seventh.flags & tickbook::kEventFlagSnapshotStart, 0);
  EXPECT_NE(seventh.flags & tickbook::kEventFlagEndOfMessage, 0);
  for (const auto& event : {first, second, third, fourth, fifth, sixth, seventh}) {
    for (const auto padding : event.padding) EXPECT_EQ(padding, 0);
  }

  auto second_config = config_for(files, files.file("second.bin"));
  second_config.trades_csv_gz = files.file("trades.csv.gz");
  const auto second_result = tickbook::convert_tardis_csv_to_binary(second_config);
  ASSERT_TRUE(second_result.success) << second_result.error;
  const auto second_bytes = read_bytes(second_config.output_file);
  EXPECT_EQ(bytes, second_bytes);
  EXPECT_EQ(fnv1a64(bytes), 135454519976258796ULL);
}

TEST(CsvToBin, RejectsPrecisionLossAndDoesNotPublishPartialOutput) {
  TempFiles files;
  const std::string book =
      "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n"
      "binance,BTCUSDT,1000,1000,true,bid,100.001,1.0\n";
  ASSERT_TRUE(write_gzip(files.file("book.csv.gz"), book));

  const auto config = config_for(files, files.file("bad.bin"));
  const auto result = tickbook::convert_tardis_csv_to_binary(config);
  EXPECT_FALSE(result.success) << result.error;
  EXPECT_NE(result.error.find("fixed-point precision"), std::string::npos) << result.error;
  EXPECT_FALSE(std::filesystem::exists(config.output_file));
  EXPECT_FALSE(std::filesystem::exists(config.output_file.string() + ".tmp"));
}

TEST(CsvToBin, ClampsBackstepsAndMergesCaptureOrderedStreamsByClampedTime) {
  TempFiles files;
  const std::string book =
      "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n"
      "binance,BTCUSDT,2000,2000,true,bid,100.00,1.0\n"
      "binance,BTCUSDT,2000,2000,true,ask,101.00,1.0\n"
      "binance,BTCUSDT,1950,2050,false,bid,100.00,2.0\n";
  const std::string trades = "exchange,symbol,timestamp,local_timestamp,id,side,price,amount\n"
                             "binance,BTCUSDT,1990,1990,1,buy,100.00,0.5\n"
                             "binance,BTCUSDT,2000,2000,2,sell,100.50,0.25\n";
  ASSERT_TRUE(write_gzip(files.file("book.csv.gz"), book));
  ASSERT_TRUE(write_gzip(files.file("trades.csv.gz"), trades));

  auto config = config_for(files, files.file("clamped.bin"));
  config.trades_csv_gz = files.file("trades.csv.gz");
  const auto result = tickbook::convert_tardis_csv_to_binary(config);
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_EQ(result.event_count, 5U);
  EXPECT_EQ(result.backstep_count, 1U);
  EXPECT_EQ(result.largest_backstep_us, 50U);

  const auto bytes = read_bytes(config.output_file);
  const auto first = read_event(bytes, 0);
  const auto second = read_event(bytes, 1);
  const auto third = read_event(bytes, 2);
  const auto fourth = read_event(bytes, 3);
  const auto fifth = read_event(bytes, 4);
  EXPECT_EQ(first.type, tickbook::EventType::Trade);
  EXPECT_EQ(first.ts_ns, 1'990'000);
  EXPECT_EQ(second.type, tickbook::EventType::SnapshotLevel);
  EXPECT_EQ(second.side, tickbook::Side::Bid);
  EXPECT_EQ(third.type, tickbook::EventType::SnapshotLevel);
  EXPECT_EQ(third.side, tickbook::Side::Ask);
  EXPECT_EQ(fourth.type, tickbook::EventType::BookUpdate);
  EXPECT_EQ(fourth.side, tickbook::Side::Bid);
  EXPECT_EQ(fifth.type, tickbook::EventType::Trade);
  EXPECT_EQ(fifth.ts_ns, 2'000'000);
  EXPECT_EQ(second.ts_ns, 2'000'000);
  EXPECT_EQ(third.ts_ns, 2'000'000);
  EXPECT_EQ(fourth.ts_ns, 2'000'000);
  EXPECT_NE(third.flags & tickbook::kEventFlagEndOfMessage, 0);
  for (std::size_t i = 1; i < result.event_count; ++i) {
    EXPECT_LE(read_event(bytes, i - 1).ts_ns, read_event(bytes, i).ts_ns);
  }

  auto repeated_config = config;
  repeated_config.output_file = files.file("clamped-again.bin");
  const auto repeated_result = tickbook::convert_tardis_csv_to_binary(repeated_config);
  ASSERT_TRUE(repeated_result.success) << repeated_result.error;
  EXPECT_EQ(read_bytes(repeated_config.output_file), bytes);
  EXPECT_EQ(repeated_result.backstep_count, result.backstep_count);
  EXPECT_EQ(repeated_result.largest_backstep_us, result.largest_backstep_us);
}

TEST(CsvToBin, AcceptsBackstepExactlyAtConfiguredLimit) {
  TempFiles files;
  const std::string book =
      "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n"
      "binance,BTCUSDT,2000,2000,true,bid,100.00,1.0\n"
      "binance,BTCUSDT,1000,2001,false,bid,100.00,2.0\n";
  ASSERT_TRUE(write_gzip(files.file("book.csv.gz"), book));

  auto config = config_for(files, files.file("at-limit.bin"));
  config.max_backstep_ms = 1;
  const auto result = tickbook::convert_tardis_csv_to_binary(config);
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_EQ(result.backstep_count, 1U);
  EXPECT_EQ(result.largest_backstep_us, 1000U);
  EXPECT_EQ(read_event(read_bytes(config.output_file), 1).ts_ns, 2'000'000);
}

TEST(CsvToBin, RejectsBackstepAboveConfiguredLimit) {
  TempFiles files;
  const std::string book =
      "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n"
      "binance,BTCUSDT,2000000,2000000,true,bid,100.00,1.0\n"
      "binance,BTCUSDT,999999,2000001,false,bid,100.00,2.0\n";
  ASSERT_TRUE(write_gzip(files.file("book.csv.gz"), book));

  const auto config = config_for(files, files.file("too-far.bin"));
  const auto result = tickbook::convert_tardis_csv_to_binary(config);
  EXPECT_FALSE(result.success) << result.error;
  EXPECT_NE(result.error.find("timestamp backstep of 1000001 us"), std::string::npos)
      << result.error;
  EXPECT_NE(result.error.find("--max-backstep-ms 1000"), std::string::npos) << result.error;
  EXPECT_FALSE(std::filesystem::exists(config.output_file));
}

TEST(CsvToBin, RequiresAnInitialSnapshot) {
  TempFiles files;
  const std::string book =
      "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n"
      "binance,BTCUSDT,1000,1000,false,bid,100.00,1.0\n";
  ASSERT_TRUE(write_gzip(files.file("book.csv.gz"), book));

  const auto config = config_for(files, files.file("no-snapshot.bin"));
  const auto result = tickbook::convert_tardis_csv_to_binary(config);
  EXPECT_FALSE(result.success) << result.error;
  EXPECT_NE(result.error.find("no snapshot rows"), std::string::npos) << result.error;
}

