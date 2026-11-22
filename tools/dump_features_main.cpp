#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string_view>
#include <system_error>

#include "dump_features.hpp"

namespace {

[[nodiscard]] bool parse_positive(std::string_view text, std::uint64_t& value) noexcept {
  if (text.empty()) return false;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value > 0;
}

void usage(std::ostream& output) {
  output << "Usage: dump_features INPUT.bin OUTPUT.tbf [--price-tick UNITS] "
            "[--sample-interval-ms MS]\n";
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      usage(std::cout);
      return 0;
    }
    if (argc < 3 || (argc - 3) % 2 != 0) {
      usage(std::cerr);
      return 2;
    }

    tickbook::feature_dump_tool::Options options{};
    options.input_path = argv[1];
    options.output_path = argv[2];
    for (int index = 3; index < argc; index += 2) {
      const std::string_view flag(argv[index]);
      const std::string_view value(argv[index + 1]);
      std::uint64_t parsed = 0;
      if (!parse_positive(value, parsed)) {
        std::cerr << "dump_features: option values must be positive integers\n";
        return 2;
      }
      if (flag == "--price-tick") {
        if (parsed > static_cast<std::uint64_t>(INT64_MAX)) {
          std::cerr << "dump_features: price tick is too large\n";
          return 2;
        }
        options.price_tick = static_cast<tickbook::Price>(parsed);
      } else if (flag == "--sample-interval-ms") {
        if (parsed > static_cast<std::uint64_t>(INT64_MAX) / 1'000'000ULL) {
          std::cerr << "dump_features: sample interval is too large\n";
          return 2;
        }
        options.sample_interval_ns = parsed * 1'000'000ULL;
      } else {
        usage(std::cerr);
        return 2;
      }
    }

    const auto result = tickbook::feature_dump_tool::dump_feature_file(options);
    if (!result.success) {
      std::cerr << "dump_features: " << result.error << '\n';
      return 1;
    }
    std::cout << "events=" << result.event_count << '\n'
              << "messages=" << result.message_count << '\n'
              << "samples=" << result.sample_count << '\n'
              << "output=" << options.output_path.string() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "dump_features: " << error.what() << '\n';
    return 1;
  }
}
