#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "tickbook/csv_line.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0 || size > 1'048'576) return 0;
  std::string line(reinterpret_cast<const char*>(data), size);
  std::array<std::string_view, tickbook::csv_detail::kCsvFieldCount> fields{};
  static_cast<void>(tickbook::csv_detail::split_csv_line(line, fields));
  return 0;
}
