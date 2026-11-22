#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "tickbook/features/feature_dump.hpp"

namespace {

tickbook::FeatureDumpHeader header() {
  tickbook::FeatureDumpHeader value{};
  value.price_scale = 2;
  value.qty_scale = 8;
  constexpr std::string_view symbol = "BTCUSDT";
  std::copy(symbol.begin(), symbol.end(), value.symbol.begin());
  value.sample_count = 1;
  return value;
}

} // namespace

TEST(FeatureDump, HeaderAndFeatureRecordRoundTripInLittleEndian) {
  const auto metadata = header();
  const auto encoded_header = tickbook::make_feature_dump_header(metadata);
  tickbook::FeatureDumpHeader decoded_header{};
  EXPECT_EQ(tickbook::validate_feature_dump_header(
                encoded_header, tickbook::kFeatureDumpHeaderSize + tickbook::kFeatureDumpRecordSize,
                decoded_header),
            tickbook::FeatureDumpError::None);
  EXPECT_EQ(decoded_header.price_scale, 2);
  EXPECT_EQ(decoded_header.qty_scale, 8);
  EXPECT_EQ(decoded_header.symbol, metadata.symbol);
  EXPECT_EQ(decoded_header.sample_count, 1U);
  EXPECT_EQ(decoded_header.sample_interval_ns, tickbook::kFeatureSampleIntervalNs);

  tickbook::FeatureDumpRecord expected{};
  expected.ts_ns = 1'700'000'000'000'000'000;
  expected.features.spread_ticks = 2;
  expected.features.imbalance_l1_q20 = -123;
  expected.features.imbalance_k_q20 = 456;
  expected.features.microprice_offset_q20_ticks = -789;
  expected.features.ofi_10 = 10;
  expected.features.ofi_50 = -50;
  expected.features.ofi_200 = 200;
  expected.features.trade_flow_100ms = -100;
  expected.features.trade_flow_1s = 1'000;
  expected.features.mid_return_100ms_q20_ticks = -2 * tickbook::kFeatureScale;
  expected.features.mid_return_1s_q20_ticks = 3 * tickbook::kFeatureScale;
  expected.forward_mid_return_q20_ticks = {4, -5, 6};
  const auto encoded_record = tickbook::encode_feature_dump_record(expected);
  EXPECT_EQ(tickbook::decode_feature_dump_record(encoded_record), expected);
  EXPECT_EQ(tickbook::encode_feature_dump_record(expected), encoded_record);
}

TEST(FeatureDump, RejectsInvalidHeaderFieldsAndCountMismatch) {
  const auto valid = tickbook::make_feature_dump_header(header());
  tickbook::FeatureDumpHeader decoded{};
  const auto file_size = tickbook::kFeatureDumpHeaderSize + tickbook::kFeatureDumpRecordSize;

  auto bad_magic = valid;
  bad_magic[0] = std::byte{0};
  EXPECT_EQ(tickbook::validate_feature_dump_header(bad_magic, file_size, decoded),
            tickbook::FeatureDumpError::WrongMagic);

  auto bad_version = valid;
  tickbook::feature_dump_detail::write_u32(std::span<std::byte>(bad_version), 8, 99);
  EXPECT_EQ(tickbook::validate_feature_dump_header(bad_version, file_size, decoded),
            tickbook::FeatureDumpError::UnsupportedVersion);

  EXPECT_EQ(tickbook::validate_feature_dump_header(valid, file_size - 1, decoded),
            tickbook::FeatureDumpError::CountMismatch);
  EXPECT_EQ(
      tickbook::validate_feature_dump_header(valid, tickbook::kFeatureDumpHeaderSize, decoded),
      tickbook::FeatureDumpError::CountMismatch);
}
