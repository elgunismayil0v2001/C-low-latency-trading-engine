#include "gen_synthetic.hpp"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <system_error>

namespace {

void usage(std::ostream& output) {
  output << "Usage: gen_synthetic --output-dir DIR [--seed N] [--messages N] [--symbol SYMBOL]\n";
}

template <typename Integer> bool parse_integer(std::string_view text, Integer& value) {
  if (text.empty()) return false;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

} // namespace

int main(int argc, char** argv) {
  tickbook::synthetic::Config config;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if (option == "--help") {
      usage(std::cout);
      return 0;
    }
    if (index + 1 >= argc) {
      usage(std::cerr);
      return 2;
    }

    const std::string_view value(argv[++index]);
    if (option == "--output-dir") {
      config.output_dir = value;
    } else if (option == "--symbol") {
      config.symbol = value;
    } else if (option == "--seed") {
      if (!parse_integer(value, config.seed)) {
        std::cerr << "invalid seed: " << value << '\n';
        return 2;
      }
    } else if (option == "--messages") {
      std::uint64_t messages = 0;
      if (!parse_integer(value, messages) ||
          messages > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        std::cerr << "invalid message count: " << value << '\n';
        return 2;
      }
      config.messages = static_cast<std::size_t>(messages);
    } else {
      std::cerr << "unknown option: " << option << '\n';
      usage(std::cerr);
      return 2;
    }
  }

  const auto result = tickbook::synthetic::generate_tardis_csv(config);
  if (!result.success) {
    std::cerr << result.error << '\n';
    return 1;
  }
  std::cout << "book_csv=" << result.book_csv_gz << '\n'
            << "trades_csv=" << result.trades_csv_gz << '\n'
            << "seed=" << config.seed << " messages=" << config.messages << '\n';
  return 0;
}
