#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <zlib.h>

#include "gen_synthetic.hpp"
#include "tickbook/binary_file.hpp"
#include "tickbook/binary_replay.hpp"
#include "tickbook/book_map.hpp"
#include "tickbook/csv_to_bin.hpp"

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence{};
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("tickbook_gen_synthetic_" + std::to_string(nonce) + "_" +
             std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] std::filesystem::path child(std::string_view name) const { return path_ / name; }

private:
  std::filesystem::path path_;
};

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

std::string read_gzip_text(const std::filesystem::path& path) {
  gzFile input = gzopen(path.string().c_str(), "rb");
  if (input == nullptr) return {};
  std::array<char, 4096> buffer{};
  std::string contents;
  for (;;) {
    const int count = gzread(input, buffer.data(), static_cast<unsigned int>(buffer.size()));
    if (count < 0) {
      gzclose(input);
      return {};
    }
    if (count == 0) break;
    contents.append(buffer.data(), static_cast<std::size_t>(count));
  }
  if (gzclose(input) != Z_OK) return {};
  return contents;
}

} // namespace

TEST(GenSynthetic, SameSeedProducesByteIdenticalTardisFilesAndBinaryReplay) {
  TemporaryDirectory files;
  constexpr std::size_t kMessages = 32;

  tickbook::synthetic::Config first_config;
  first_config.output_dir = files.child("first");
  first_config.seed = 42;
  first_config.messages = kMessages;
  const auto first = tickbook::synthetic::generate_tardis_csv(first_config);
  ASSERT_TRUE(first.success) << first.error;

  auto second_config = first_config;
  second_config.output_dir = files.child("second");
  const auto second = tickbook::synthetic::generate_tardis_csv(second_config);
  ASSERT_TRUE(second.success) << second.error;
  EXPECT_EQ(read_bytes(first.book_csv_gz), read_bytes(second.book_csv_gz));
  EXPECT_EQ(read_bytes(first.trades_csv_gz), read_bytes(second.trades_csv_gz));

  auto different_seed_config = first_config;
  different_seed_config.output_dir = files.child("different_seed");
  ++different_seed_config.seed;
  const auto different_seed = tickbook::synthetic::generate_tardis_csv(different_seed_config);
  ASSERT_TRUE(different_seed.success) << different_seed.error;
  EXPECT_NE(read_bytes(first.book_csv_gz), read_bytes(different_seed.book_csv_gz));

  const auto book_csv = read_gzip_text(first.book_csv_gz);
  const auto trades_csv = read_gzip_text(first.trades_csv_gz);
  ASSERT_FALSE(book_csv.empty());
  ASSERT_FALSE(trades_csv.empty());
  EXPECT_NE(
      book_csv.find("exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n"),
      std::string::npos);
  EXPECT_NE(book_csv.find(",true,bid,"), std::string::npos);
  EXPECT_NE(book_csv.find(",false,"), std::string::npos);
  EXPECT_NE(book_csv.find(",false,bid,100.00,0.00000000\n"), std::string::npos);
  EXPECT_NE(trades_csv.find("exchange,symbol,timestamp,local_timestamp,id,side,price,amount\n"),
            std::string::npos);
  EXPECT_NE(trades_csv.find(",buy,"), std::string::npos);
  EXPECT_NE(trades_csv.find(",sell,"), std::string::npos);
  EXPECT_EQ(std::count(book_csv.begin(), book_csv.end(), '\n'), 1 + 10 + kMessages * 2);
  EXPECT_EQ(std::count(trades_csv.begin(), trades_csv.end(), '\n'), 1 + kMessages);

  tickbook::CsvToBinaryConfig first_binary_config;
  first_binary_config.book_csv_gz = first.book_csv_gz;
  first_binary_config.trades_csv_gz = first.trades_csv_gz;
  first_binary_config.output_file = files.child("first.bin");
  const auto first_binary = tickbook::convert_tardis_csv_to_binary(first_binary_config);
  ASSERT_TRUE(first_binary.success) << first_binary.error;
  EXPECT_EQ(first_binary.event_count, 10 + kMessages * 3);

  tickbook::CsvToBinaryConfig second_binary_config = first_binary_config;
  second_binary_config.book_csv_gz = second.book_csv_gz;
  second_binary_config.trades_csv_gz = second.trades_csv_gz;
  second_binary_config.output_file = files.child("second.bin");
  const auto second_binary = tickbook::convert_tardis_csv_to_binary(second_binary_config);
  ASSERT_TRUE(second_binary.success) << second_binary.error;
  EXPECT_EQ(read_bytes(first_binary_config.output_file),
            read_bytes(second_binary_config.output_file));

  const auto binary_bytes = read_bytes(first_binary_config.output_file);
  tickbook::BinaryFileView view;
  ASSERT_EQ(tickbook::validate_binary_file(binary_bytes, view), tickbook::BinaryFileError::None);
  EXPECT_EQ(view.event_count, 10 + kMessages * 3);
  EXPECT_EQ(view.price_scale, 2);
  EXPECT_EQ(view.qty_scale, 8);

  tickbook::BinaryReplay replay;
  std::string error;
  ASSERT_TRUE(replay.open(first_binary_config.output_file, error)) << error;
  tickbook::BookMap book;
  std::size_t message_count = 0;
  while (const auto event_view = replay.next()) {
    const auto event = event_view->copy();
    if ((event.flags & tickbook::kEventFlagSnapshotStart) != 0) book.clear();
    book.apply(event);
    if ((event.flags & tickbook::kEventFlagEndOfMessage) != 0) {
      EXPECT_FALSE(book.crossed()) << "crossed at generated message " << message_count;
      ++message_count;
    }
  }
  EXPECT_EQ(message_count, kMessages + 1);
}

TEST(GenSynthetic, RejectsInvalidConfiguration) {
  TemporaryDirectory files;
  tickbook::synthetic::Config config;
  config.output_dir = files.child("zero_messages");
  config.messages = 0;
  EXPECT_FALSE(tickbook::synthetic::generate_tardis_csv(config).success);

  config.messages = 10;
  config.symbol = "BTC,USDT";
  EXPECT_FALSE(tickbook::synthetic::generate_tardis_csv(config).success);
}
