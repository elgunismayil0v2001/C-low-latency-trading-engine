#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace tickbook::synthetic {

struct Config {
  std::filesystem::path output_dir;
  std::string symbol{"BTCUSDT"};
  std::uint64_t seed{1};
  std::size_t messages{100};
};

struct Result {
  bool success{};
  std::filesystem::path book_csv_gz;
  std::filesystem::path trades_csv_gz;
  std::string error;
};

// Write deterministic synthetic Tardis-format gzip CSVs for converter tests and replay.
[[nodiscard]] Result generate_tardis_csv(const Config& config);

} // namespace tickbook::synthetic
