#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string_view>
#include <system_error>

#include "label_features.hpp"

namespace {

[[nodiscard]] bool parse_positive(std::string_view text, std::uint64_t& value) noexcept {
  if (text.empty()) return false;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value > 0;
}

void usage(std::ostream& output) {
  output << "Usage: label_features INPUT.tbf OUTPUT.tfl --kind direction|return --horizon 1|3|10 "
            "[--dead-zone-ticks K]\n";
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      usage(std::cout);
      return 0;
    }
    if (argc < 7 || (argc - 3) % 2 != 0) {
      usage(std::cerr);
      return 2;
    }
    tickbook::label_tool::Options options{};
    options.input_path = argv[1];
    options.output_path = argv[2];
    std::string_view kind;
    std::int64_t dead_zone_ticks = 1;
    for (int index = 3; index < argc; index += 2) {
      const std::string_view flag(argv[index]);
      const std::string_view value(argv[index + 1]);
      if (flag == "--kind") {
        kind = value;
      } else if (flag == "--horizon") {
        std::uint64_t parsed = 0;
        if (!parse_positive(value, parsed) || parsed > UINT32_MAX) {
          std::cerr << "label_features: horizon must be a positive integer\n";
          return 2;
        }
        options.horizon_seconds = static_cast<std::uint32_t>(parsed);
      } else if (flag == "--dead-zone-ticks") {
        std::uint64_t parsed = 0;
        if (!parse_positive(value, parsed) || parsed > INT64_MAX) {
          std::cerr << "label_features: dead zone must be a positive integer\n";
          return 2;
        }
        dead_zone_ticks = static_cast<std::int64_t>(parsed);
      } else {
        usage(std::cerr);
        return 2;
      }
    }

    tickbook::label_tool::DirectionLabeler direction(dead_zone_ticks);
    tickbook::label_tool::ReturnLabeler returns;
    if (kind == "direction") options.labeler = &direction;
    else if (kind == "return") options.labeler = &returns;
    else {
      std::cerr << "label_features: kind must be direction or return\n";
      return 2;
    }
    if (kind == "return" && dead_zone_ticks != 1) {
      std::cerr << "label_features: --dead-zone-ticks applies only to direction labels\n";
      return 2;
    }

    const auto result = tickbook::label_tool::label_feature_file(options);
    if (!result.success) {
      std::cerr << "label_features: " << result.error << '\n';
      return 1;
    }
    std::cout << "samples=" << result.sample_count << '\n'
              << "output=" << options.output_path.string() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "label_features: " << error.what() << '\n';
    return 1;
  }
}
