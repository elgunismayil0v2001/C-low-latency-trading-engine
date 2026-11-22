#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "tickbook/features/feature_dump.hpp"

namespace tickbook::feature_dump_tool {

struct Options {
  std::filesystem::path input_path;
  std::filesystem::path output_path;
  Price price_tick{1};
  std::uint64_t sample_interval_ns{kFeatureSampleIntervalNs};
};

struct Result {
  bool success{};
  std::uint64_t event_count{};
  std::uint64_t message_count{};
  std::uint64_t sample_count{};
  std::string error;
};

[[nodiscard]] Result dump_feature_file(const Options& options);

} // namespace tickbook::feature_dump_tool
