#include <cstddef>
#include <cstdint>
#include <string_view>

#include "tickbook/fixed.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) return 0;
  const int scale = static_cast<int>(data[0] % 20U) - 1;
  const auto* text = reinterpret_cast<const char*>(data + 1);
  const std::string_view input(text, size - 1);
  std::int64_t parsed = 0;
  static_cast<void>(tickbook::parse_fixed(input, scale, parsed));
  return 0;
}
