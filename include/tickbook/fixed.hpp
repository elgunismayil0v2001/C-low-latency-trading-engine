#pragma once
#include <cstdint>
#include <limits>
#include <string_view>

namespace tickbook {

// Parse a non-negative decimal string ("43000.12", "0.00000100") into a fixed-point
// integer with `scale` fractional digits, without using floating point.
// Returns false on bad input, overflow, or non-zero digits beyond `scale`.
// Trailing zeros beyond `scale` are accepted (Binance pads to 8 decimals).
[[nodiscard]] inline bool parse_fixed(std::string_view s, int scale, std::int64_t& out) noexcept {
  if (s.empty() || scale < 0 || scale > 18) return false;
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  std::int64_t value = 0;
  int frac_digits = 0;
  bool seen_dot = false;
  bool seen_digit = false;
  for (const char c : s) {
    if (c == '.') {
      if (seen_dot) return false;
      seen_dot = true;
      continue;
    }
    if (c < '0' || c > '9') return false;
    seen_digit = true;
    if (seen_dot && ++frac_digits > scale) {
      if (c != '0') return false; // would lose precision
      continue;
    }
    const int d = c - '0';
    if (value > (kMax - d) / 10) return false;
    value = value * 10 + d;
  }
  if (!seen_digit) return false;
  for (int i = (frac_digits > scale ? scale : frac_digits); i < scale; ++i) {
    if (value > kMax / 10) return false;
    value *= 10;
  }
  out = value;
  return true;
}

} // namespace tickbook
