#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace tickbook {

struct CsvToBinaryConfig {
  std::filesystem::path book_csv_gz;
  std::optional<std::filesystem::path> trades_csv_gz;
  std::filesystem::path output_file;
  std::string symbol{"BTCUSDT"};
  std::int32_t price_scale{2};
  std::int32_t qty_scale{8};
};

struct CsvToBinaryResult {
  bool success{};
  std::uint64_t event_count{};
  std::string error;
};

// Stream Tardis gzip CSV rows into the versioned binary event format.
[[nodiscard]] CsvToBinaryResult convert_tardis_csv_to_binary(const CsvToBinaryConfig& config);

} // namespace tickbook
