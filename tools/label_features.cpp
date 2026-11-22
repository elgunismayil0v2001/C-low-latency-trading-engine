#include "label_features.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <system_error>

namespace tickbook::label_tool {
namespace {

[[nodiscard]] bool horizon_index(std::uint32_t seconds, std::size_t& index) noexcept {
  for (std::size_t candidate = 0; candidate < kFeatureForwardHorizonsNs.size(); ++candidate) {
    if (seconds == kFeatureForwardHorizonsNs[candidate] / 1'000'000'000ULL) {
      index = candidate;
      return true;
    }
  }
  return false;
}

void remove_temporary(const std::filesystem::path& path) noexcept {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

} // namespace

DirectionLabeler::DirectionLabeler(std::int64_t dead_zone_ticks) noexcept
    : dead_zone_ticks_(dead_zone_ticks) {
  constexpr auto kScale = static_cast<std::uint64_t>(kFeatureScale);
  if (dead_zone_ticks <= 0 ||
      static_cast<std::uint64_t>(dead_zone_ticks) >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / kScale) {
    return;
  }
  dead_zone_q20_ = dead_zone_ticks * kFeatureScale;
  valid_ = true;
}

std::int64_t DirectionLabeler::label(const FeatureDumpRecord& record,
                                     std::size_t horizon_index_value) const noexcept {
  if (!valid_ || horizon_index_value >= record.forward_mid_return_q20_ticks.size()) return -1;
  const auto value = record.forward_mid_return_q20_ticks[horizon_index_value];
  if (value > -dead_zone_q20_ && value < dead_zone_q20_) return 1;
  return value < 0 ? 0 : 2;
}

std::int64_t ReturnLabeler::label(const FeatureDumpRecord& record,
                                  std::size_t horizon_index_value) const noexcept {
  if (horizon_index_value >= record.forward_mid_return_q20_ticks.size()) return 0;
  return record.forward_mid_return_q20_ticks[horizon_index_value];
}

Result label_feature_file(const Options& options) {
  Result result{};
  std::size_t selected_horizon = 0;
  if (options.input_path.empty() || options.output_path.empty() || options.labeler == nullptr) {
    result.error = "input, output and labeler are required";
    return result;
  }
  if (!horizon_index(options.horizon_seconds, selected_horizon)) {
    result.error = "horizon must be 1, 3 or 10 seconds";
    return result;
  }
  if (const auto* direction = dynamic_cast<const DirectionLabeler*>(options.labeler);
      direction != nullptr && !direction->valid()) {
    result.error = "direction dead zone must be positive and representable";
    return result;
  }

  std::error_code filesystem_error;
  const auto file_size = std::filesystem::file_size(options.input_path, filesystem_error);
  if (filesystem_error) {
    result.error = "cannot stat feature dump: " + filesystem_error.message();
    return result;
  }
  std::ifstream input(options.input_path, std::ios::binary);
  if (!input) {
    result.error = "cannot open feature dump: " + options.input_path.string();
    return result;
  }

  std::array<std::byte, kFeatureDumpHeaderSize> source_header_bytes{};
  input.read(reinterpret_cast<char*>(source_header_bytes.data()),
             static_cast<std::streamsize>(source_header_bytes.size()));
  if (!input) {
    result.error = "cannot read feature dump header";
    return result;
  }
  FeatureDumpHeader source_header{};
  const auto source_header_error =
      validate_feature_dump_header(source_header_bytes, file_size, source_header);
  if (source_header_error != FeatureDumpError::None) {
    result.error = std::string(feature_dump_error_message(source_header_error));
    return result;
  }

  auto temporary_path = options.output_path;
  temporary_path += ".tmp";
  remove_temporary(temporary_path);
  std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
  if (!output) {
    result.error = "cannot create label dump: " + temporary_path.string();
    return result;
  }

  LabelDumpHeader destination_header{};
  destination_header.kind = options.labeler->kind();
  destination_header.horizon_seconds = options.horizon_seconds;
  destination_header.dead_zone_ticks = options.labeler->dead_zone_ticks();
  destination_header.sample_count = source_header.sample_count;
  destination_header.symbol = source_header.symbol;
  const auto destination_header_bytes = make_label_dump_header(destination_header);
  output.write(reinterpret_cast<const char*>(destination_header_bytes.data()),
               static_cast<std::streamsize>(destination_header_bytes.size()));

  std::array<std::byte, kFeatureDumpRecordSize> source_record_bytes{};
  std::int64_t previous_timestamp = -1;
  for (std::uint64_t row_index = 0; row_index < source_header.sample_count; ++row_index) {
    input.read(reinterpret_cast<char*>(source_record_bytes.data()),
               static_cast<std::streamsize>(source_record_bytes.size()));
    if (!input) {
      result.error = "feature dump record is truncated";
      break;
    }
    const auto source_record = decode_feature_dump_record(source_record_bytes);
    if (source_record.ts_ns < 0 || source_record.ts_ns <= previous_timestamp) {
      result.error = "feature dump timestamps are negative or not strictly increasing";
      break;
    }
    previous_timestamp = source_record.ts_ns;
    const LabelDumpRecord destination_record{
        source_record.ts_ns, source_record.features,
        options.labeler->label(source_record, selected_horizon)};
    if (destination_header.kind == LabelKind::Direction &&
        (destination_record.label < 0 || destination_record.label > 2)) {
      result.error = "labeler returned an invalid direction class";
      break;
    }
    const auto encoded_record = encode_label_dump_record(destination_record);
    output.write(reinterpret_cast<const char*>(encoded_record.data()),
                 static_cast<std::streamsize>(encoded_record.size()));
    if (!output) {
      result.error = "cannot write label dump record";
      break;
    }
    ++result.sample_count;
  }

  if (!result.error.empty()) {
    output.close();
    remove_temporary(temporary_path);
    return result;
  }
  output.flush();
  if (!output) {
    result.error = "cannot flush label dump";
    output.close();
    remove_temporary(temporary_path);
    return result;
  }
  output.close();
  if (!output) {
    result.error = "cannot close label dump";
    remove_temporary(temporary_path);
    return result;
  }
  std::filesystem::rename(temporary_path, options.output_path, filesystem_error);
  if (filesystem_error) {
    result.error = "cannot publish label dump: " + filesystem_error.message();
    remove_temporary(temporary_path);
    return result;
  }
  result.success = true;
  return result;
}

} // namespace tickbook::label_tool
