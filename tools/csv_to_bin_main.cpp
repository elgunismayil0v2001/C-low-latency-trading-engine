#include <charconv>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

#include "tickbook/csv_to_bin.hpp"

namespace {

void usage(std::ostream& output) {
  output << "Usage: csv_to_bin --book FILE.csv.gz [--trades FILE.csv.gz] --output FILE.bin "
            "[--symbol BTCUSDT] [--price-scale 2] [--qty-scale 8] "
            "[--max-backstep-ms 1000]\n";
}

bool parse_scale(std::string_view text, std::int32_t& value) {
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_max_backstep(std::string_view text, std::uint64_t& value) {
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
         value <= std::numeric_limits<std::uint64_t>::max() / 1000U;
}

} // namespace

int main(int argc, char** argv) {
  tickbook::CsvToBinaryConfig config;
  bool have_book = false;
  bool have_output = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view option(argv[i]);
    if (option == "--help") {
      usage(std::cout);
      return 0;
    }
    if (i + 1 >= argc) {
      usage(std::cerr);
      return 2;
    }
    const std::string_view value(argv[++i]);
    if (option == "--book") {
      config.book_csv_gz = value;
      have_book = true;
    } else if (option == "--trades") {
      config.trades_csv_gz = value;
    } else if (option == "--output") {
      config.output_file = value;
      have_output = true;
    } else if (option == "--symbol") {
      config.symbol = value;
    } else if (option == "--price-scale") {
      if (!parse_scale(value, config.price_scale)) {
        std::cerr << "invalid price scale\n";
        return 2;
      }
    } else if (option == "--qty-scale") {
      if (!parse_scale(value, config.qty_scale)) {
        std::cerr << "invalid quantity scale\n";
        return 2;
      }
    } else if (option == "--max-backstep-ms") {
      if (!parse_max_backstep(value, config.max_backstep_ms)) {
        std::cerr << "invalid maximum timestamp backstep\n";
        return 2;
      }
    } else {
      std::cerr << "unknown option: " << option << '\n';
      usage(std::cerr);
      return 2;
    }
  }

  if (!have_book || !have_output) {
    usage(std::cerr);
    return 2;
  }

  const auto result = tickbook::convert_tardis_csv_to_binary(config);
  if (!result.success) {
    std::cerr << "csv_to_bin: " << result.error << '\n';
    return 1;
  }
  std::cout << "wrote " << result.event_count << " events to " << config.output_file.string()
            << '\n'
            << "timestamp_backsteps=" << result.backstep_count
            << " largest_backstep_us=" << result.largest_backstep_us
            << " max_backstep_ms=" << config.max_backstep_ms << '\n';
  return 0;
}
