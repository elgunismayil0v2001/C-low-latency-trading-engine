#include <iomanip>
#include <iostream>
#include <optional>
#include <string_view>

#include "tickbook/replay.hpp"

namespace {

void usage(std::ostream& output) {
  output << "Usage: replay FILE.bin [--lock-pages] [--pin-threads]\n";
}

void print_level(std::string_view name, const std::optional<tickbook::Level>& level) {
  std::cout << name << '=';
  if (level) std::cout << level->price << ':' << level->qty;
  else std::cout << "none";
  std::cout << '\n';
}

} // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    usage(std::cout);
    return 0;
  }
  if (argc < 2) {
    usage(std::cerr);
    return 2;
  }

  tickbook::ReplayOptions options{};
  for (int index = 2; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if (option == "--lock-pages") options.lock_pages = true;
    else if (option == "--pin-threads") options.pin_threads = true;
    else {
      usage(std::cerr);
      return 2;
    }
  }

  const auto result = tickbook::run_binary_replay(argv[1], options);
  if (!result.success) {
    std::cerr << "replay: " << result.error << '\n';
    return 1;
  }

  std::cout << "symbol=";
  for (const char character : result.symbol) {
    if (character == '\0') break;
    std::cout << character;
  }
  std::cout << '\n'
            << "price_scale=" << result.price_scale << '\n'
            << "qty_scale=" << result.qty_scale << '\n'
            << "events=" << result.event_count << '\n'
            << "bid_levels=" << result.bid_levels << '\n'
            << "ask_levels=" << result.ask_levels << '\n';
  print_level("best_bid", result.best_bid);
  print_level("best_ask", result.best_ask);
  std::cout << "book_checksum=" << std::hex << std::setw(16) << std::setfill('0')
            << result.book_checksum << '\n';
  return 0;
}
