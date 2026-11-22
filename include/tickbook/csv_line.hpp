#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace tickbook::csv_detail {

inline constexpr std::size_t kCsvFieldCount = 8;

// Decode one CSV record in place so returned views stay within the line's storage.
[[nodiscard]] inline bool
split_csv_line(std::string& line, std::array<std::string_view, kCsvFieldCount>& fields) noexcept {
  std::size_t read = 0;
  std::size_t write = 0;
  std::size_t count = 0;

  for (;;) {
    if (count == fields.size()) return false;
    const std::size_t start = write;
    if (read < line.size() && line[read] == '"') {
      ++read;
      bool closed = false;
      while (read < line.size()) {
        const char character = line[read++];
        if (character != '"') {
          line[write++] = character;
          continue;
        }
        if (read < line.size() && line[read] == '"') {
          line[write++] = '"';
          ++read;
          continue;
        }
        closed = true;
        break;
      }
      if (!closed || (read < line.size() && line[read] != ',')) return false;
    } else {
      while (read < line.size() && line[read] != ',') {
        if (line[read] == '"') return false;
        line[write++] = line[read++];
      }
    }
    fields[count++] = std::string_view(line.data() + start, write - start);
    if (read == line.size()) return count == fields.size();
    ++read; // comma
    if (read == line.size() && count == fields.size() - 1) {
      fields[count++] = std::string_view(line.data() + write, 0);
      return count == fields.size();
    }
  }
}

} // namespace tickbook::csv_detail
