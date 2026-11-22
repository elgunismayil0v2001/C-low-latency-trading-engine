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
#include <system_error>
#include <vector>

#include "label_features.hpp"

namespace {

class TempFiles {
public:
  TempFiles() {
    static std::atomic<std::uint64_t> sequence{};
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ = std::filesystem::temp_directory_path() /
                 ("tickbook_label_features_" + std::to_string(nonce) + "_" +
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

bool write_features(const std::filesystem::path& path) {
  tickbook::FeatureDumpHeader header{};
  header.sample_count = 5;
  constexpr std::string_view symbol = "BTCUSDT";
  std::copy(symbol.begin(), symbol.end(), header.symbol.begin());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  const auto encoded_header = tickbook::make_feature_dump_header(header);
  output.write(reinterpret_cast<const char*>(encoded_header.data()),
               static_cast<std::streamsize>(encoded_header.size()));
  constexpr std::array<std::int64_t, 5> returns{tickbook::kFeatureScale / 2,
                                                tickbook::kFeatureScale, -tickbook::kFeatureScale,
                                                0, 2 * tickbook::kFeatureScale};
  for (std::size_t index = 0; index < returns.size(); ++index) {
    tickbook::FeatureDumpRecord row{};
    row.ts_ns = 1'600'000'000'000'000'000LL + static_cast<std::int64_t>(index) * 100'000'000LL;
    row.features.spread_ticks = static_cast<std::int64_t>(index + 1);
    row.features.imbalance_l1_q20 = static_cast<std::int32_t>(index * 100);
    row.forward_mid_return_q20_ticks[1] = returns[index];
    const auto encoded_record = tickbook::encode_feature_dump_record(row);
    output.write(reinterpret_cast<const char*>(encoded_record.data()),
                 static_cast<std::streamsize>(encoded_record.size()));
  }
  return output.good();
}

bool read_label_dump(const std::filesystem::path& path, tickbook::LabelDumpHeader& header,
                     std::vector<tickbook::LabelDumpRecord>& records) {
  std::error_code error;
  const auto file_size = std::filesystem::file_size(path, error);
  if (error) return false;
  std::ifstream input(path, std::ios::binary);
  std::array<std::byte, tickbook::kLabelDumpHeaderSize> header_bytes{};
  input.read(reinterpret_cast<char*>(header_bytes.data()),
             static_cast<std::streamsize>(header_bytes.size()));
  if (!input || tickbook::validate_label_dump_header(header_bytes, file_size, header) !=
                    tickbook::LabelDumpError::None) {
    return false;
  }
  std::array<std::byte, tickbook::kLabelDumpRecordSize> record_bytes{};
  for (std::uint64_t index = 0; index < header.sample_count; ++index) {
    input.read(reinterpret_cast<char*>(record_bytes.data()),
               static_cast<std::streamsize>(record_bytes.size()));
    if (!input) return false;
    records.push_back(tickbook::decode_label_dump_record(record_bytes));
  }
  return true;
}

} // namespace

TEST(LabelFeatures, DirectionLabelUsesStrictOneTickDeadZoneAndClassOrder) {
  tickbook::label_tool::DirectionLabeler labeler(1);
  ASSERT_TRUE(labeler.valid());
  tickbook::FeatureDumpRecord row{};
  row.forward_mid_return_q20_ticks[0] = tickbook::kFeatureScale - 1;
  EXPECT_EQ(labeler.label(row, 0), 1);
  row.forward_mid_return_q20_ticks[0] = tickbook::kFeatureScale;
  EXPECT_EQ(labeler.label(row, 0), 2);
  row.forward_mid_return_q20_ticks[0] = -tickbook::kFeatureScale;
  EXPECT_EQ(labeler.label(row, 0), 0);
  row.forward_mid_return_q20_ticks[0] = 0;
  EXPECT_EQ(labeler.label(row, 0), 1);
  EXPECT_FALSE(tickbook::label_tool::DirectionLabeler(0).valid());
}

TEST(LabelFeatures, WritesVersionedDirectionAndReturnDumpsWithKindMetadata) {
  TempFiles files;
  ASSERT_TRUE(write_features(files.file("features.tbf")));

  tickbook::label_tool::DirectionLabeler direction(1);
  const auto direction_result = tickbook::label_tool::label_feature_file(
      {files.file("features.tbf"), files.file("direction.tfl"), &direction, 3});
  ASSERT_TRUE(direction_result.success) << direction_result.error;
  EXPECT_EQ(direction_result.sample_count, 5U);
  tickbook::LabelDumpHeader direction_header{};
  std::vector<tickbook::LabelDumpRecord> direction_records;
  ASSERT_TRUE(read_label_dump(files.file("direction.tfl"), direction_header, direction_records));
  EXPECT_EQ(direction_header.kind, tickbook::LabelKind::Direction);
  EXPECT_EQ(direction_header.horizon_seconds, 3U);
  EXPECT_EQ(direction_header.dead_zone_ticks, 1);
  ASSERT_EQ(direction_records.size(), 5U);
  EXPECT_EQ(direction_records[0].label, 1);
  EXPECT_EQ(direction_records[1].label, 2);
  EXPECT_EQ(direction_records[2].label, 0);
  EXPECT_EQ(direction_records[3].label, 1);
  EXPECT_EQ(direction_records[4].label, 2);
  EXPECT_EQ(direction_records[4].features.spread_ticks, 5);

  tickbook::label_tool::ReturnLabeler returns;
  const auto return_result = tickbook::label_tool::label_feature_file(
      {files.file("features.tbf"), files.file("returns.tfl"), &returns, 3});
  ASSERT_TRUE(return_result.success) << return_result.error;
  tickbook::LabelDumpHeader return_header{};
  std::vector<tickbook::LabelDumpRecord> return_records;
  ASSERT_TRUE(read_label_dump(files.file("returns.tfl"), return_header, return_records));
  EXPECT_EQ(return_header.kind, tickbook::LabelKind::Return);
  EXPECT_EQ(return_header.dead_zone_ticks, 0);
  EXPECT_EQ(return_records[0].label, tickbook::kFeatureScale / 2);
  EXPECT_EQ(return_records[2].label, -tickbook::kFeatureScale);
}

TEST(LabelFeatures, RejectsUnsupportedHorizonWithoutPublishingOutput) {
  TempFiles files;
  ASSERT_TRUE(write_features(files.file("features.tbf")));
  tickbook::label_tool::ReturnLabeler labeler;
  const auto result = tickbook::label_tool::label_feature_file(
      {files.file("features.tbf"), files.file("bad.tfl"), &labeler, 2});
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error, "horizon must be 1, 3 or 10 seconds");
  EXPECT_FALSE(std::filesystem::exists(files.file("bad.tfl")));
}
