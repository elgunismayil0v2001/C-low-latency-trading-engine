#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace tickbook::signal_eval {

struct Result {
  bool success{};
  std::uint64_t sample_count{};
  std::uint64_t out_of_sample_count{};
  std::string report;
  std::string error;
};

[[nodiscard]] Result evaluate_feature_file(const std::filesystem::path& path);

} // namespace tickbook::signal_eval
