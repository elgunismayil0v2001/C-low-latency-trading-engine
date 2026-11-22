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

#include "eval_signals.hpp"
#include "tickbook/features/feature_dump.hpp"

namespace {

class TempFiles {
public:
  TempFiles() {
    static std::atomic<std::uint64_t> sequence{};
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ =
        std::filesystem::temp_directory_path() / ("tickbook_eval_signals_" + std::to_string(nonce) +
                                                  "_" + std::to_string(sequence.fetch_add(1)));
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

bool write_dataset(const std::filesystem::path& path, std::size_t count) {
  tickbook::FeatureDumpHeader header{};
  header.sample_count = count;
  constexpr std::string_view symbol = "BTCUSDT";
  std::copy(symbol.begin(), symbol.end(), header.symbol.begin());
  const auto encoded_header = tickbook::make_feature_dump_header(header);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(encoded_header.data()),
               static_cast<std::streamsize>(encoded_header.size()));
  for (std::size_t index = 0; index < count; ++index) {
    tickbook::FeatureDumpRecord row{};
    row.ts_ns = 1'600'000'000'000'000'000LL + static_cast<std::int64_t>(index) * 100'000'000LL;
    const auto sign = index % 2 == 0 ? -1 : 1;
    row.features.imbalance_l1_q20 = static_cast<std::int32_t>(sign * tickbook::kFeatureScale);
    row.features.imbalance_k_q20 = row.features.imbalance_l1_q20;
    row.features.ofi_10 = sign;
    for (std::size_t horizon = 0; horizon < row.forward_mid_return_q20_ticks.size(); ++horizon) {
      row.forward_mid_return_q20_ticks[horizon] =
          static_cast<std::int64_t>(sign) * tickbook::kFeatureScale;
    }
    const auto bytes = tickbook::encode_feature_dump_record(row);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  return output.good();
}

} // namespace

TEST(EvalSignals, ReportsWalkForwardFeaturesBaselinesAndHourlyBlocks) {
  TempFiles files;
  ASSERT_TRUE(write_dataset(files.file("features.tbf"), 180));

  const auto result = tickbook::signal_eval::evaluate_feature_file(files.file("features.tbf"));
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_EQ(result.sample_count, 180U);
  EXPECT_GT(result.out_of_sample_count, 0U);
  EXPECT_NE(result.report.find("#walk_forward=expanding_50_67_83_percent_no_shuffle"),
            std::string::npos);
  EXPECT_NE(result.report.find("#purge_rule=exclude training rows"), std::string::npos);
  EXPECT_NE(result.report.find("feature,imbalance_l1,1,overall"), std::string::npos);
  EXPECT_NE(result.report.find("baseline,sign_imbalance_l1,3,overall"), std::string::npos);
  EXPECT_NE(result.report.find("baseline,logistic_all_features,10,overall"), std::string::npos);
  EXPECT_NE(result.report.find("2020-09-13T12:00Z"), std::string::npos);
  const auto logistic_row = result.report.find("baseline,logistic_all_features,1,overall,");
  ASSERT_NE(logistic_row, std::string::npos);
  const auto row_end = result.report.find('\n', logistic_row);
  const auto row = result.report.substr(logistic_row, row_end - logistic_row);
  std::size_t comma_count = 0;
  auto field_start = std::size_t{0};
  std::string information_coefficient;
  for (std::size_t position = 0; position <= row.size(); ++position) {
    if (position == row.size() || row[position] == ',') {
      if (comma_count == 6) {
        information_coefficient = row.substr(field_start, position - field_start);
        break;
      }
      ++comma_count;
      field_start = position + 1;
    }
  }
  EXPECT_FALSE(information_coefficient.empty());
}

TEST(EvalSignals, RejectsMalformedFeatureDumpAndTooFewSamples) {
  TempFiles files;
  ASSERT_TRUE(write_dataset(files.file("small.tbf"), 20));
  const auto small = tickbook::signal_eval::evaluate_feature_file(files.file("small.tbf"));
  EXPECT_FALSE(small.success);
  EXPECT_NE(small.error.find("at least 30"), std::string::npos);

  std::ofstream corrupt(files.file("corrupt.tbf"), std::ios::binary | std::ios::trunc);
  corrupt << "not a feature dump";
  corrupt.close();
  const auto invalid = tickbook::signal_eval::evaluate_feature_file(files.file("corrupt.tbf"));
  EXPECT_FALSE(invalid.success);
}
