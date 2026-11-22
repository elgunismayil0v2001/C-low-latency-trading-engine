#include "gen_synthetic.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <zlib.h>

namespace tickbook::synthetic {
namespace {

constexpr std::uint64_t kBaseTimestampUs = 1'700'000'000'000'000ULL;
constexpr std::size_t kMaxMessages = 1'000'000;
constexpr std::uint64_t kInitialMidTicks = 10'000; // 100.00 at price scale 2.

class SplitMix64 {
public:
  explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] std::uint64_t next() noexcept {
    std::uint64_t value = (state_ += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
  }

private:
  std::uint64_t state_;
};

class GzipCsvWriter {
public:
  explicit GzipCsvWriter(const std::filesystem::path& path)
      : file_(gzopen(path.string().c_str(), "wb9")) {}
  GzipCsvWriter(const GzipCsvWriter&) = delete;
  GzipCsvWriter& operator=(const GzipCsvWriter&) = delete;

  ~GzipCsvWriter() {
    if (file_ != nullptr) static_cast<void>(gzclose(file_));
  }

  [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }

  [[nodiscard]] bool write_line(std::string_view line) noexcept {
    if (file_ == nullptr ||
        line.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return false;
    }
    const auto size = static_cast<unsigned int>(line.size());
    return gzwrite(file_, line.data(), size) == static_cast<int>(size);
  }

  [[nodiscard]] bool close() noexcept {
    if (file_ == nullptr) return false;
    const int result = gzclose(file_);
    file_ = nullptr;
    return result == Z_OK;
  }

private:
  gzFile file_{};
};

class RemoveOnFailure {
public:
  RemoveOnFailure(std::filesystem::path first, std::filesystem::path second)
      : first_(std::move(first)), second_(std::move(second)) {}
  ~RemoveOnFailure() {
    if (!keep_) {
      std::error_code ignored;
      std::filesystem::remove(first_, ignored);
      std::filesystem::remove(second_, ignored);
    }
  }
  void keep() noexcept { keep_ = true; }

private:
  std::filesystem::path first_;
  std::filesystem::path second_;
  bool keep_{};
};

[[nodiscard]] bool valid_symbol(std::string_view symbol) noexcept {
  if (symbol.empty() || symbol.size() > 16) return false;
  for (const char character : symbol) {
    if (!((character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9'))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string fixed_decimal(std::uint64_t value, unsigned int scale) {
  std::uint64_t divisor = 1;
  for (unsigned int i = 0; i < scale; ++i) divisor *= 10;
  std::string result = std::to_string(value / divisor);
  result.push_back('.');
  const std::string fraction = std::to_string(value % divisor);
  result.append(scale - fraction.size(), '0');
  result.append(fraction);
  return result;
}

[[nodiscard]] std::uint64_t quantity_units(SplitMix64& random) noexcept {
  return 10'000'000ULL + random.next() % 990'000'001ULL;
}

[[nodiscard]] bool write_book_row(GzipCsvWriter& output, std::string_view symbol,
                                  std::uint64_t timestamp_us, std::uint64_t local_timestamp_us,
                                  bool snapshot, std::string_view side, std::uint64_t price_ticks,
                                  std::uint64_t amount_units) {
  std::string line;
  line.reserve(112 + symbol.size());
  line.append("binance,");
  line.append(symbol);
  line.push_back(',');
  line.append(std::to_string(timestamp_us));
  line.push_back(',');
  line.append(std::to_string(local_timestamp_us));
  line.push_back(',');
  line.append(snapshot ? "true," : "false,");
  line.append(side);
  line.push_back(',');
  line.append(fixed_decimal(price_ticks, 2));
  line.push_back(',');
  line.append(fixed_decimal(amount_units, 8));
  line.push_back('\n');
  return output.write_line(line);
}

[[nodiscard]] bool write_trade_row(GzipCsvWriter& output, std::string_view symbol,
                                   std::uint64_t timestamp_us, std::uint64_t local_timestamp_us,
                                   std::uint64_t trade_id, std::string_view side,
                                   std::uint64_t price_ticks, std::uint64_t amount_units) {
  std::string line;
  line.reserve(112 + symbol.size());
  line.append("binance,");
  line.append(symbol);
  line.push_back(',');
  line.append(std::to_string(timestamp_us));
  line.push_back(',');
  line.append(std::to_string(local_timestamp_us));
  line.push_back(',');
  line.append(std::to_string(trade_id));
  line.push_back(',');
  line.append(side);
  line.push_back(',');
  line.append(fixed_decimal(price_ticks, 2));
  line.push_back(',');
  line.append(fixed_decimal(amount_units, 8));
  line.push_back('\n');
  return output.write_line(line);
}

[[nodiscard]] Result failure(std::string message) {
  Result result;
  result.error = std::move(message);
  return result;
}

} // namespace

Result generate_tardis_csv(const Config& config) {
  if (config.output_dir.empty()) return failure("output directory is required");
  if (!valid_symbol(config.symbol)) {
    return failure("symbol must contain 1 to 16 uppercase letters or digits");
  }
  if (config.messages == 0 || config.messages > kMaxMessages) {
    return failure("message count must be in [1, 1000000]");
  }

  std::error_code filesystem_error;
  std::filesystem::create_directories(config.output_dir, filesystem_error);
  if (filesystem_error)
    return failure("cannot create output directory: " + filesystem_error.message());

  const auto book_path = config.output_dir / "incremental_book_L2.csv.gz";
  const auto trades_path = config.output_dir / "trades.csv.gz";
  auto book_temp_path = book_path;
  auto trades_temp_path = trades_path;
  book_temp_path += ".tmp";
  trades_temp_path += ".tmp";
  RemoveOnFailure cleanup(book_temp_path, trades_temp_path);

  GzipCsvWriter book(book_temp_path);
  GzipCsvWriter trades(trades_temp_path);
  if (!book.is_open() || !trades.is_open())
    return failure("cannot create synthetic gzip CSV files");

  constexpr std::string_view kBookHeader =
      "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n";
  constexpr std::string_view kTradesHeader =
      "exchange,symbol,timestamp,local_timestamp,id,side,price,amount\n";
  if (!book.write_line(kBookHeader) || !trades.write_line(kTradesHeader)) {
    return failure("cannot write synthetic CSV headers");
  }

  SplitMix64 random(config.seed);
  for (std::uint64_t distance = 0; distance < 5; ++distance) {
    const auto bid_price = kInitialMidTicks - distance;
    const auto ask_price = kInitialMidTicks + distance + 1;
    if (!write_book_row(book, config.symbol, kBaseTimestampUs, kBaseTimestampUs, true, "bid",
                        bid_price, quantity_units(random)) ||
        !write_book_row(book, config.symbol, kBaseTimestampUs, kBaseTimestampUs, true, "ask",
                        ask_price, quantity_units(random))) {
      return failure("cannot write synthetic snapshot rows");
    }
  }

  std::int64_t mid_ticks = static_cast<std::int64_t>(kInitialMidTicks);
  for (std::size_t message = 0; message < config.messages; ++message) {
    const auto movement = random.next() % 3;
    mid_ticks += movement == 0 ? -1 : movement == 1 ? 0 : 1;
    if (mid_ticks < 9'000) mid_ticks = 9'000;
    if (mid_ticks > 11'000) mid_ticks = 11'000;
    const auto timestamp_us = kBaseTimestampUs + (message + 1) * 1'000ULL;
    const auto local_timestamp_us = timestamp_us + 250;

    const bool remove_bid = message % 13 == 0;
    const auto moving_bid =
        static_cast<std::uint64_t>(mid_ticks - 1 - static_cast<std::int64_t>(random.next() % 5));
    const auto bid_price =
        remove_bid ? kInitialMidTicks
                   : (moving_bid < kInitialMidTicks - 1 ? moving_bid : kInitialMidTicks - 1);
    const auto bid_amount = remove_bid ? 0 : quantity_units(random);
    const bool remove_ask = message % 17 == 0;
    const auto moving_ask =
        static_cast<std::uint64_t>(mid_ticks + 1 + static_cast<std::int64_t>(random.next() % 5));
    const auto ask_price =
        remove_ask ? kInitialMidTicks + 1
                   : (moving_ask > kInitialMidTicks + 1 ? moving_ask : kInitialMidTicks + 1);
    const auto ask_amount = remove_ask ? 0 : quantity_units(random);
    if (!write_book_row(book, config.symbol, timestamp_us, local_timestamp_us, false, "bid",
                        bid_price, bid_amount) ||
        !write_book_row(book, config.symbol, timestamp_us, local_timestamp_us, false, "ask",
                        ask_price, ask_amount)) {
      return failure("cannot write synthetic book updates");
    }

    const auto trade_side = message % 2 == 0 ? "buy" : "sell";
    if (!write_trade_row(trades, config.symbol, timestamp_us, local_timestamp_us, message + 1,
                         trade_side, static_cast<std::uint64_t>(mid_ticks),
                         quantity_units(random))) {
      return failure("cannot write synthetic trade rows");
    }
  }

  if (!book.close() || !trades.close()) return failure("cannot finalize synthetic gzip CSV files");
  std::filesystem::rename(book_temp_path, book_path, filesystem_error);
  if (filesystem_error)
    return failure("cannot publish synthetic book CSV: " + filesystem_error.message());
  std::filesystem::rename(trades_temp_path, trades_path, filesystem_error);
  if (filesystem_error)
    return failure("cannot publish synthetic trades CSV: " + filesystem_error.message());

  cleanup.keep();
  Result result;
  result.success = true;
  result.book_csv_gz = book_path;
  result.trades_csv_gz = trades_path;
  return result;
}

} // namespace tickbook::synthetic
