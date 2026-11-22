#include "eval_signals.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "tickbook/features/feature_dump.hpp"

namespace tickbook::signal_eval {
namespace {

constexpr std::size_t kFeatureCount = 11;
constexpr std::size_t kWalkForwardFoldCount = 3;
constexpr std::size_t kMaximumLogisticTrainingRows = 30'000;
constexpr std::size_t kLogisticIterations = 80;
constexpr double kLogisticLearningRate = 0.1;
constexpr double kLogisticRegularization = 1e-3;
constexpr std::array<std::size_t, kWalkForwardFoldCount + 1> kSplitPercentages{50, 67, 83, 100};

using FeatureRow = std::array<std::int64_t, kFeatureCount>;

struct Dataset {
  FeatureDumpHeader header{};
  std::vector<FeatureDumpRecord> rows;
};

struct HourBlock {
  std::string label;
  std::vector<std::size_t> indices;
};

struct Statistics {
  std::size_t samples{};
  std::size_t nonflat{};
  std::size_t hits{};
  double information_coefficient{std::numeric_limits<double>::quiet_NaN()};

  [[nodiscard]] double hit_rate() const noexcept {
    return nonflat == 0 ? std::numeric_limits<double>::quiet_NaN()
                        : static_cast<double>(hits) / static_cast<double>(nonflat);
  }
};

struct LogisticModel {
  std::array<double, kFeatureCount> mean{};
  std::array<double, kFeatureCount> scale{};
  std::array<double, kFeatureCount> weights{};
  double bias{};
  bool trained{};
  std::size_t training_rows{};
  std::size_t purged_rows{};
};

struct LogisticTrainingFold {
  std::size_t horizon{};
  std::size_t train_end{};
  std::size_t test_start{};
  std::size_t test_end{};
};

[[nodiscard]] FeatureRow feature_values(const FeatureVector& feature) noexcept {
  return {feature.spread_ticks,
          feature.imbalance_l1_q20,
          feature.imbalance_k_q20,
          feature.microprice_offset_q20_ticks,
          feature.ofi_10,
          feature.ofi_50,
          feature.ofi_200,
          feature.trade_flow_100ms,
          feature.trade_flow_1s,
          feature.mid_return_100ms_q20_ticks,
          feature.mid_return_1s_q20_ticks};
}

[[nodiscard]] std::int64_t target_value(const FeatureDumpRecord& row,
                                        std::size_t horizon) noexcept {
  return row.forward_mid_return_q20_ticks[horizon];
}

[[nodiscard]] double logistic_probability(const LogisticModel& model,
                                          const FeatureVector& feature) noexcept {
  const auto values = feature_values(feature);
  double linear = model.bias;
  for (std::size_t index = 0; index < values.size(); ++index) {
    linear += model.weights[index] *
              ((static_cast<double>(values[index]) - model.mean[index]) / model.scale[index]);
  }
  if (linear >= 30.0) return 1.0;
  if (linear <= -30.0) return 0.0;
  return 1.0 / (1.0 + std::exp(-linear));
}

[[nodiscard]] LogisticModel train_logistic(const std::vector<FeatureDumpRecord>& rows,
                                           const LogisticTrainingFold& fold) {
  LogisticModel model{};
  const auto [horizon, train_end, test_start, test_end] = fold;
  if (test_start >= rows.size() || test_start >= test_end) return model;
  const auto first_test_ts = rows[test_start].ts_ns;
  const auto horizon_ns = kFeatureForwardHorizonsNs[horizon];
  std::vector<std::size_t> eligible;
  eligible.reserve(train_end);
  for (std::size_t index = 0; index < train_end; ++index) {
    const auto start = static_cast<std::uint64_t>(rows[index].ts_ns);
    const bool label_crosses_split =
        start > std::numeric_limits<std::uint64_t>::max() - horizon_ns ||
        start + horizon_ns >= static_cast<std::uint64_t>(first_test_ts);
    if (label_crosses_split) {
      ++model.purged_rows;
    } else {
      eligible.push_back(index);
    }
  }
  if (eligible.empty()) return model;

  const auto stride = std::max<std::size_t>(
      1, (eligible.size() + kMaximumLogisticTrainingRows - 1) / kMaximumLogisticTrainingRows);
  std::vector<std::size_t> selected;
  selected.reserve(std::min(eligible.size(), kMaximumLogisticTrainingRows));
  for (std::size_t position = 0; position < eligible.size(); position += stride) {
    selected.push_back(eligible[position]);
  }
  if (selected.empty()) return model;

  std::array<long double, kFeatureCount> means{};
  std::array<long double, kFeatureCount> squared_differences{};
  for (const auto index : selected) {
    const auto values = feature_values(rows[index].features);
    for (std::size_t column = 0; column < values.size(); ++column) {
      means[column] += static_cast<long double>(values[column]);
    }
  }
  for (auto& mean : means) mean /= static_cast<long double>(selected.size());
  for (const auto index : selected) {
    const auto values = feature_values(rows[index].features);
    for (std::size_t column = 0; column < values.size(); ++column) {
      const auto difference = static_cast<long double>(values[column]) - means[column];
      squared_differences[column] += difference * difference;
    }
  }
  for (std::size_t column = 0; column < kFeatureCount; ++column) {
    model.mean[column] = static_cast<double>(means[column]);
    const auto variance = squared_differences[column] / static_cast<long double>(selected.size());
    const auto standard_deviation = std::sqrt(static_cast<double>(variance));
    model.scale[column] = standard_deviation > 0.0 ? standard_deviation : 1.0;
  }

  std::vector<std::size_t> directional;
  directional.reserve(selected.size());
  bool saw_up = false;
  bool saw_down = false;
  for (const auto index : selected) {
    const auto target = target_value(rows[index], horizon);
    if (target == 0) continue;
    directional.push_back(index);
    saw_up = saw_up || target > 0;
    saw_down = saw_down || target < 0;
  }
  if (directional.size() < 10 || !saw_up || !saw_down) return model;

  std::array<double, kFeatureCount> gradient{};
  double bias_gradient = 0.0;
  for (std::size_t iteration = 0; iteration < kLogisticIterations; ++iteration) {
    gradient.fill(0.0);
    bias_gradient = 0.0;
    for (const auto index : directional) {
      const auto values = feature_values(rows[index].features);
      double linear = model.bias;
      std::array<double, kFeatureCount> normalized{};
      for (std::size_t column = 0; column < values.size(); ++column) {
        normalized[column] =
            (static_cast<double>(values[column]) - model.mean[column]) / model.scale[column];
        linear += model.weights[column] * normalized[column];
      }
      if (linear >= 30.0) linear = 30.0;
      else if (linear <= -30.0) linear = -30.0;
      const auto probability = 1.0 / (1.0 + std::exp(-linear));
      const auto label = target_value(rows[index], horizon) > 0 ? 1.0 : 0.0;
      const auto difference = probability - label;
      bias_gradient += difference;
      for (std::size_t column = 0; column < values.size(); ++column) {
        gradient[column] += difference * normalized[column];
      }
    }
    const auto reciprocal_count = 1.0 / static_cast<double>(directional.size());
    model.bias -= kLogisticLearningRate * bias_gradient * reciprocal_count;
    for (std::size_t column = 0; column < kFeatureCount; ++column) {
      const auto regularized =
          gradient[column] * reciprocal_count + kLogisticRegularization * model.weights[column];
      model.weights[column] -= kLogisticLearningRate * regularized;
    }
  }
  model.trained = true;
  model.training_rows = directional.size();
  return model;
}

[[nodiscard]] std::vector<double> ranks_for(const std::vector<FeatureDumpRecord>& rows,
                                            std::span<const std::size_t> indices,
                                            std::size_t horizon,
                                            std::optional<std::size_t> feature_index) {
  std::vector<std::pair<std::int64_t, std::size_t>> ordered;
  ordered.reserve(indices.size());
  for (std::size_t position = 0; position < indices.size(); ++position) {
    const auto& row = rows[indices[position]];
    const auto value =
        feature_index ? feature_values(row.features)[*feature_index] : target_value(row, horizon);
    ordered.emplace_back(value, position);
  }
  std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
    if (left.first != right.first) return left.first < right.first;
    return left.second < right.second;
  });
  std::vector<double> ranks(indices.size());
  std::size_t begin = 0;
  while (begin < ordered.size()) {
    auto end = begin + 1;
    while (end < ordered.size() && ordered[end].first == ordered[begin].first) ++end;
    const auto average_rank = (static_cast<double>(begin + 1) + static_cast<double>(end)) / 2.0;
    for (auto position = begin; position < end; ++position) {
      ranks[ordered[position].second] = average_rank;
    }
    begin = end;
  }
  return ranks;
}

[[nodiscard]] std::vector<double> ranks_for_scores(std::span<const std::size_t> indices,
                                                   std::span<const double> scores) {
  std::vector<std::pair<double, std::size_t>> ordered;
  ordered.reserve(indices.size());
  for (std::size_t position = 0; position < indices.size(); ++position) {
    ordered.emplace_back(scores[indices[position]], position);
  }
  std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
    if (left.first != right.first) return left.first < right.first;
    return left.second < right.second;
  });
  std::vector<double> ranks(indices.size());
  std::size_t begin = 0;
  while (begin < ordered.size()) {
    auto end = begin + 1;
    while (end < ordered.size() && ordered[end].first == ordered[begin].first) ++end;
    const auto average_rank = (static_cast<double>(begin + 1) + static_cast<double>(end)) / 2.0;
    for (auto position = begin; position < end; ++position) {
      ranks[ordered[position].second] = average_rank;
    }
    begin = end;
  }
  return ranks;
}

[[nodiscard]] double correlation(const std::vector<double>& left,
                                 const std::vector<double>& right) noexcept {
  if (left.size() < 2 || left.size() != right.size()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const auto count = static_cast<double>(left.size());
  const auto mean_left = std::accumulate(left.begin(), left.end(), 0.0) / count;
  const auto mean_right = std::accumulate(right.begin(), right.end(), 0.0) / count;
  double covariance = 0.0;
  double variance_left = 0.0;
  double variance_right = 0.0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto delta_left = left[index] - mean_left;
    const auto delta_right = right[index] - mean_right;
    covariance += delta_left * delta_right;
    variance_left += delta_left * delta_left;
    variance_right += delta_right * delta_right;
  }
  const auto denominator = std::sqrt(variance_left * variance_right);
  return denominator == 0.0 ? std::numeric_limits<double>::quiet_NaN() : covariance / denominator;
}

[[nodiscard]] std::array<Statistics, kFeatureCount>
feature_statistics(const std::vector<FeatureDumpRecord>& rows, std::span<const std::size_t> indices,
                   std::size_t horizon) {
  std::array<Statistics, kFeatureCount> output{};
  if (indices.empty()) return output;
  const auto target_ranks = ranks_for(rows, indices, horizon, std::nullopt);
  for (std::size_t feature = 0; feature < kFeatureCount; ++feature) {
    auto& statistics = output[feature];
    statistics.samples = indices.size();
    const auto feature_ranks = ranks_for(rows, indices, horizon, feature);
    statistics.information_coefficient = correlation(feature_ranks, target_ranks);
    for (const auto index : indices) {
      const auto target = target_value(rows[index], horizon);
      if (target == 0) continue;
      ++statistics.nonflat;
      const auto value = feature_values(rows[index].features)[feature];
      if ((value > 0 && target > 0) || (value < 0 && target < 0)) ++statistics.hits;
    }
  }
  return output;
}

[[nodiscard]] Statistics logistic_statistics(const std::vector<FeatureDumpRecord>& rows,
                                             std::span<const std::size_t> indices,
                                             std::span<const std::int8_t> predictions,
                                             std::span<const double> scores, std::size_t horizon) {
  Statistics statistics{};
  std::vector<std::size_t> scored_indices;
  scored_indices.reserve(indices.size());
  for (const auto index : indices) {
    if (!std::isfinite(scores[index])) continue;
    ++statistics.samples;
    scored_indices.push_back(index);
    const auto target = target_value(rows[index], horizon);
    if (target == 0 || predictions[index] < 0) continue;
    ++statistics.nonflat;
    if ((predictions[index] == 1 && target > 0) || (predictions[index] == 0 && target < 0)) {
      ++statistics.hits;
    }
  }
  if (!scored_indices.empty()) {
    const auto score_ranks = ranks_for_scores(scored_indices, scores);
    const auto target_ranks = ranks_for(rows, scored_indices, horizon, std::nullopt);
    statistics.information_coefficient = correlation(score_ranks, target_ranks);
  }
  return statistics;
}

[[nodiscard]] std::string hour_block(std::int64_t ts_ns) {
  const auto seconds = static_cast<std::time_t>(ts_ns / 1'000'000'000LL);
  std::tm utc{};
  if (::gmtime_r(&seconds, &utc) == nullptr) return "unknown-hour";
  std::array<char, 32> buffer{};
  if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:00Z", &utc) == 0) {
    return "unknown-hour";
  }
  return buffer.data();
}

[[nodiscard]] bool read_dataset(const std::filesystem::path& path, Dataset& dataset,
                                std::string& error) {
  std::error_code filesystem_error;
  const auto file_size = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error) {
    error = "cannot stat feature dump: " + filesystem_error.message();
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "cannot open feature dump: " + path.string();
    return false;
  }
  std::array<std::byte, kFeatureDumpHeaderSize> header_bytes{};
  input.read(reinterpret_cast<char*>(header_bytes.data()),
             static_cast<std::streamsize>(header_bytes.size()));
  if (!input) {
    error = "cannot read feature dump header";
    return false;
  }
  const auto header_error = validate_feature_dump_header(header_bytes, file_size, dataset.header);
  if (header_error != FeatureDumpError::None) {
    error = std::string(feature_dump_error_message(header_error));
    return false;
  }
  if (dataset.header.sample_count >
      std::numeric_limits<std::size_t>::max() / sizeof(FeatureDumpRecord)) {
    error = "feature dump contains too many samples for this process";
    return false;
  }
  dataset.rows.reserve(static_cast<std::size_t>(dataset.header.sample_count));
  std::array<std::byte, kFeatureDumpRecordSize> record_bytes{};
  std::int64_t previous_ts = -1;
  for (std::uint64_t index = 0; index < dataset.header.sample_count; ++index) {
    input.read(reinterpret_cast<char*>(record_bytes.data()),
               static_cast<std::streamsize>(record_bytes.size()));
    if (!input) {
      error = "feature dump record is truncated";
      return false;
    }
    const auto row = decode_feature_dump_record(record_bytes);
    if (row.ts_ns < 0 || row.ts_ns <= previous_ts) {
      error = "feature dump timestamps are negative or not strictly increasing";
      return false;
    }
    previous_ts = row.ts_ns;
    dataset.rows.push_back(row);
  }
  return true;
}

[[nodiscard]] std::string number(double value) {
  if (!std::isfinite(value)) return {};
  std::ostringstream output;
  output << std::fixed << std::setprecision(6) << value;
  return output.str();
}

void append_csv_row(std::ostringstream& output, std::string_view metric, std::string_view feature,
                    std::size_t horizon, std::string_view block, const Statistics& statistics,
                    std::size_t training_rows = 0, std::size_t purged_rows = 0,
                    std::size_t trained_folds = 0) {
  output << metric << ',' << feature << ',' << kFeatureForwardHorizonsNs[horizon] / 1'000'000'000ULL
         << ',' << block << ',' << statistics.samples << ',' << statistics.nonflat << ','
         << number(statistics.information_coefficient) << ',' << number(statistics.hit_rate())
         << ',' << training_rows << ',' << purged_rows << ',' << trained_folds << '\n';
}

[[nodiscard]] std::string symbol_string(const std::array<char, 16>& symbol) {
  const auto end = std::find(symbol.begin(), symbol.end(), '\0');
  return std::string(symbol.begin(), end);
}

} // namespace

Result evaluate_feature_file(const std::filesystem::path& path) {
  Result result{};
  Dataset dataset;
  if (!read_dataset(path, dataset, result.error)) return result;
  result.sample_count = dataset.rows.size();
  if (dataset.rows.size() < 30) {
    result.error = "feature dump needs at least 30 complete samples for walk-forward evaluation";
    return result;
  }

  const auto& rows = dataset.rows;
  const auto count = rows.size();
  std::array<std::vector<std::size_t>, 3> test_indices{};
  std::array<std::vector<std::int8_t>, 3> logistic_predictions{std::vector<std::int8_t>(count, -1),
                                                               std::vector<std::int8_t>(count, -1),
                                                               std::vector<std::int8_t>(count, -1)};
  std::array<std::vector<double>, 3> logistic_scores{
      std::vector<double>(count, std::numeric_limits<double>::quiet_NaN()),
      std::vector<double>(count, std::numeric_limits<double>::quiet_NaN()),
      std::vector<double>(count, std::numeric_limits<double>::quiet_NaN())};
  std::array<std::size_t, 3> total_training_rows{};
  std::array<std::size_t, 3> total_purged_rows{};
  std::array<std::size_t, 3> used_folds{};

  for (std::size_t fold = 0; fold < kWalkForwardFoldCount; ++fold) {
    const auto train_end = count * kSplitPercentages[fold] / 100U;
    const auto test_end = count * kSplitPercentages[fold + 1] / 100U;
    if (train_end >= test_end || train_end >= count) continue;
    for (std::size_t horizon = 0; horizon < kFeatureForwardHorizonsNs.size(); ++horizon) {
      auto model = train_logistic(rows, {horizon, train_end, train_end, test_end});
      total_training_rows[horizon] += model.training_rows;
      total_purged_rows[horizon] += model.purged_rows;
      for (std::size_t index = train_end; index < test_end; ++index) {
        test_indices[horizon].push_back(index);
        if (model.trained) {
          const auto score = logistic_probability(model, rows[index].features);
          logistic_scores[horizon][index] = score;
          logistic_predictions[horizon][index] = score >= 0.5 ? 1 : 0;
        }
      }
      if (model.trained) ++used_folds[horizon];
    }
  }

  for (const auto& indices : test_indices) result.out_of_sample_count += indices.size();
  if (result.out_of_sample_count == 0) {
    result.error = "walk-forward splits produced no out-of-sample rows";
    return result;
  }

  std::ostringstream report;
  report << "#symbol=" << symbol_string(dataset.header.symbol) << '\n'
         << "#samples=" << result.sample_count << '\n'
         << "#out_of_sample_rows_per_horizon=" << test_indices[0].size() << '\n'
         << "#walk_forward=expanding_50_67_83_percent_no_shuffle\n"
         << "#purge_rule=exclude training rows with ts+horizon >= first test timestamp\n"
         << "#logistic=deterministic_full_batch_gradient_descent_temporally_thinned_to_30000_rows\n"
         << "metric,feature,horizon_s,block,samples,nonflat,ic_spearman,hit_rate,"
            "train_directional_samples,purged_train_rows,trained_folds\n";

  static constexpr std::array<std::string_view, kFeatureCount> feature_names{
      "spread_ticks",  "imbalance_l1",     "imbalance_k",  "microprice_offset",
      "ofi_10",        "ofi_50",           "ofi_200",      "trade_flow_100ms",
      "trade_flow_1s", "mid_return_100ms", "mid_return_1s"};

  for (std::size_t horizon = 0; horizon < kFeatureForwardHorizonsNs.size(); ++horizon) {
    const auto& indices = test_indices[horizon];
    const auto overall_features = feature_statistics(rows, indices, horizon);
    for (std::size_t feature = 0; feature < kFeatureCount; ++feature) {
      append_csv_row(report, "feature", feature_names[feature], horizon, "overall",
                     overall_features[feature]);
    }
    append_csv_row(report, "baseline", "sign_imbalance_l1", horizon, "overall",
                   overall_features[1]);
    append_csv_row(report, "baseline", "logistic_all_features", horizon, "overall",
                   logistic_statistics(rows, indices, logistic_predictions[horizon],
                                       logistic_scores[horizon], horizon),
                   total_training_rows[horizon], total_purged_rows[horizon], used_folds[horizon]);

    std::vector<HourBlock> blocks;
    for (const auto index : indices) {
      const auto block = hour_block(rows[index].ts_ns);
      if (blocks.empty() || blocks.back().label != block) blocks.push_back({block, {}});
      blocks.back().indices.push_back(index);
    }
    for (const auto& block : blocks) {
      const auto block_features = feature_statistics(rows, block.indices, horizon);
      for (std::size_t feature = 0; feature < kFeatureCount; ++feature) {
        append_csv_row(report, "feature", feature_names[feature], horizon, block.label,
                       block_features[feature]);
      }
      append_csv_row(report, "baseline", "sign_imbalance_l1", horizon, block.label,
                     block_features[1]);
      append_csv_row(report, "baseline", "logistic_all_features", horizon, block.label,
                     logistic_statistics(rows, block.indices, logistic_predictions[horizon],
                                         logistic_scores[horizon], horizon),
                     total_training_rows[horizon], total_purged_rows[horizon], used_folds[horizon]);
    }
  }

  result.report = report.str();
  result.success = true;
  return result;
}

} // namespace tickbook::signal_eval
