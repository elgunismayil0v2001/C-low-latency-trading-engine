#include "tickbook/csv_to_bin.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <zlib.h>

#include "tickbook/binary_file.hpp"
#include "tickbook/csv_line.hpp"
#include "tickbook/fixed.hpp"
#include "tickbook/types.hpp"

namespace tickbook {
namespace {

constexpr std::size_t kGzipBufferSize = 16 * 1024;

enum class CsvKind { Book, Trades };
enum class ReadStatus { Row, End, Error };

struct CsvRecord {
  std::uint64_t timestamp_us{};
  std::uint64_t local_timestamp_us{};
  Event event{};
};

class GzipLineReader {
public:
  explicit GzipLineReader(const std::filesystem::path& path) : path_(path.string()) {
    file_ = gzopen(path_.c_str(), "rb");
    if (file_ == nullptr) error_ = "cannot open gzip CSV: " + path_;
  }

  GzipLineReader(const GzipLineReader&) = delete;
  GzipLineReader& operator=(const GzipLineReader&) = delete;

  ~GzipLineReader() {
    if (file_ != nullptr) gzclose(file_);
  }

  [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

  bool next_line(std::string& line) {
    line.clear();
    if (eof_ || file_ == nullptr) return false;

    for (;;) {
      if (buffer_pos_ == buffer_size_) {
        const int count = gzread(file_, buffer_.data(), static_cast<unsigned int>(buffer_.size()));
        if (count < 0) {
          int code = Z_OK;
          const char* message = gzerror(file_, &code);
          error_ = "gzip read failed for " + path_ + ": " +
                   (message == nullptr ? "unknown zlib error" : message);
          return false;
        }
        if (count == 0) {
          eof_ = true;
          if (!gzeof(file_)) {
            int code = Z_OK;
            const char* message = gzerror(file_, &code);
            error_ = "gzip read failed for " + path_ + ": " +
                     (message == nullptr ? "unexpected end of input" : message);
            return false;
          }
          if (line.empty()) return false;
          break;
        }
        buffer_pos_ = 0;
        buffer_size_ = static_cast<std::size_t>(count);
      }

      const char character = buffer_[buffer_pos_++];
      if (character == '\n') break;
      line.push_back(character);
    }

    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
  }

private:
  std::string path_;
  gzFile file_{};
  std::array<char, kGzipBufferSize> buffer_{};
  std::size_t buffer_pos_{};
  std::size_t buffer_size_{};
  bool eof_{};
  std::string error_;
};

template <typename Integer> bool parse_unsigned(std::string_view text, Integer& value) {
  if (text.empty()) return false;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_timestamp(std::string_view text, std::uint64_t& timestamp_us) {
  return parse_unsigned(text, timestamp_us) &&
         timestamp_us <=
             static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 1000);
}

class TardisCsvReader {
public:
  TardisCsvReader(const std::filesystem::path& path, CsvKind kind, std::string_view symbol,
                  std::int32_t price_scale, std::int32_t qty_scale)
      : input_(path), kind_(kind), symbol_(symbol), price_scale_(price_scale),
        qty_scale_(qty_scale) {}

  [[nodiscard]] bool is_open() const noexcept { return input_.is_open(); }
  [[nodiscard]] const std::string& error() const noexcept {
    return error_.empty() ? input_.error() : error_;
  }

  ReadStatus next(CsvRecord& record) {
    std::string line;
    for (;;) {
      if (!input_.next_line(line)) {
        if (!input_.error().empty()) {
          error_ = input_.error();
          return ReadStatus::Error;
        }
        if (!header_seen_) {
          error_ = "CSV is missing its header";
          return ReadStatus::Error;
        }
        if (kind_ == CsvKind::Book && !seen_snapshot_) {
          error_ = "book CSV contains no snapshot rows";
          return ReadStatus::Error;
        }
        return ReadStatus::End;
      }
      ++line_number_;
      if (line.empty()) continue;

      std::array<std::string_view, csv_detail::kCsvFieldCount> fields{};
      if (!csv_detail::split_csv_line(line, fields)) {
        error_ = row_error("invalid CSV row");
        return ReadStatus::Error;
      }
      if (!header_seen_) {
        if (!valid_header(fields)) {
          error_ = row_error("unexpected CSV header");
          return ReadStatus::Error;
        }
        header_seen_ = true;
        continue;
      }

      if (fields[0] != "binance" || fields[1] != symbol_) {
        error_ = row_error("exchange or symbol does not match converter configuration");
        return ReadStatus::Error;
      }

      CsvRecord parsed{};
      if (!parse_timestamp(fields[2], parsed.timestamp_us) ||
          !parse_unsigned(fields[3], parsed.local_timestamp_us)) {
        error_ = row_error("invalid timestamp");
        return ReadStatus::Error;
      }
      if (have_previous_timestamp_ && parsed.timestamp_us < previous_timestamp_us_) {
        error_ = row_error("timestamp moved backwards; input rows must be capture ordered");
        return ReadStatus::Error;
      }
      previous_timestamp_us_ = parsed.timestamp_us;
      have_previous_timestamp_ = true;

      parsed.event.ts_ns = static_cast<std::int64_t>(parsed.timestamp_us * 1000U);
      parsed.event.first_id = 0;
      parsed.event.last_id = 0;
      if (!parse_fixed(fields[6], price_scale_, parsed.event.price) ||
          !parse_fixed(fields[7], qty_scale_, parsed.event.qty)) {
        error_ = row_error("price or amount is invalid or loses configured fixed-point precision");
        return ReadStatus::Error;
      }

      if (kind_ == CsvKind::Trades) {
        if (fields[5] == "buy") parsed.event.side = Side::Bid;
        else if (fields[5] == "sell") parsed.event.side = Side::Ask;
        else {
          error_ = row_error("trade side must be buy or sell");
          return ReadStatus::Error;
        }
        parsed.event.type = EventType::Trade;
        record = parsed;
        return ReadStatus::Row;
      }

      bool is_snapshot = false;
      if (fields[4] == "true") is_snapshot = true;
      else if (fields[4] != "false") {
        error_ = row_error("is_snapshot must be true or false");
        return ReadStatus::Error;
      }
      if (fields[5] == "bid") parsed.event.side = Side::Bid;
      else if (fields[5] == "ask") parsed.event.side = Side::Ask;
      else {
        error_ = row_error("book side must be bid or ask");
        return ReadStatus::Error;
      }

      if (!seen_snapshot_ && !is_snapshot) continue;
      if (is_snapshot && !previous_is_snapshot_) { parsed.event.flags |= kEventFlagSnapshotStart; }
      parsed.event.type = is_snapshot ? EventType::SnapshotLevel : EventType::BookUpdate;
      seen_snapshot_ = true;
      previous_is_snapshot_ = is_snapshot;
      record = parsed;
      return ReadStatus::Row;
    }
  }

private:
  bool valid_header(const std::array<std::string_view, csv_detail::kCsvFieldCount>& fields) const {
    constexpr std::array<std::string_view, csv_detail::kCsvFieldCount> kBookHeader{
        "exchange",    "symbol", "timestamp", "local_timestamp",
        "is_snapshot", "side",   "price",     "amount"};
    constexpr std::array<std::string_view, csv_detail::kCsvFieldCount> kTradesHeader{
        "exchange", "symbol", "timestamp", "local_timestamp", "id", "side", "price", "amount"};
    return fields == (kind_ == CsvKind::Book ? kBookHeader : kTradesHeader);
  }

  [[nodiscard]] std::string row_error(std::string_view message) const {
    return "CSV line " + std::to_string(line_number_) + ": " + std::string(message);
  }

  GzipLineReader input_;
  CsvKind kind_;
  std::string symbol_;
  std::int32_t price_scale_;
  std::int32_t qty_scale_;
  std::string error_;
  std::uint64_t line_number_{};
  std::uint64_t previous_timestamp_us_{};
  bool header_seen_{};
  bool have_previous_timestamp_{};
  bool seen_snapshot_{};
  bool previous_is_snapshot_{};
};

void write_u64_le(std::array<std::byte, kBinaryEventSize>& bytes, std::size_t offset,
                  std::uint64_t value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    bytes[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xffU);
  }
}

std::array<std::byte, kBinaryEventSize> encode_event(const Event& event) {
  std::array<std::byte, kBinaryEventSize> bytes{};
  write_u64_le(bytes, 0, std::bit_cast<std::uint64_t>(event.ts_ns));
  write_u64_le(bytes, 8, event.first_id);
  write_u64_le(bytes, 16, event.last_id);
  write_u64_le(bytes, 24, std::bit_cast<std::uint64_t>(event.price));
  write_u64_le(bytes, 32, std::bit_cast<std::uint64_t>(event.qty));
  bytes[40] = static_cast<std::byte>(event.type);
  bytes[41] = static_cast<std::byte>(event.side);
  bytes[42] = static_cast<std::byte>(event.flags);
  for (std::size_t i = 0; i < std::size(event.padding); ++i) {
    bytes[43 + i] = static_cast<std::byte>(event.padding[i]);
  }
  return bytes;
}

bool write_event(std::ofstream& output, const Event& event) {
  const auto bytes = encode_event(event);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return output.good();
}

CsvToBinaryResult failure(std::string message) {
  return CsvToBinaryResult{false, 0, std::move(message)};
}

} // namespace

CsvToBinaryResult convert_tardis_csv_to_binary(const CsvToBinaryConfig& config) {
  if (config.book_csv_gz.empty() || config.output_file.empty()) {
    return failure("book input and output paths are required");
  }
  if (config.symbol.empty() || config.symbol.size() > 16) {
    return failure("symbol must contain between 1 and 16 bytes");
  }
  if (config.price_scale < 0 || config.price_scale > 18 || config.qty_scale < 0 ||
      config.qty_scale > 18) {
    return failure("price and quantity scales must be in [0, 18]");
  }

  TardisCsvReader book(config.book_csv_gz, CsvKind::Book, config.symbol, config.price_scale,
                       config.qty_scale);
  if (!book.is_open()) return failure(book.error());

  std::optional<TardisCsvReader> trades;
  if (config.trades_csv_gz) {
    trades.emplace(*config.trades_csv_gz, CsvKind::Trades, config.symbol, config.price_scale,
                   config.qty_scale);
    if (!trades->is_open()) return failure(trades->error());
  }

  std::array<char, 16> symbol{};
  std::copy(config.symbol.begin(), config.symbol.end(), symbol.begin());

  auto temporary_file = config.output_file;
  temporary_file += ".tmp";
  std::ofstream output(temporary_file, std::ios::binary | std::ios::trunc);
  if (!output) return failure("cannot create output file: " + temporary_file.string());
  const auto discard_temporary = [&temporary_file]() {
    std::error_code ignored;
    std::filesystem::remove(temporary_file, ignored);
  };

  const BinaryFileHeaderBytes empty_header{};
  output.write(reinterpret_cast<const char*>(empty_header.data()),
               static_cast<std::streamsize>(empty_header.size()));
  if (!output) {
    output.close();
    discard_temporary();
    return failure("cannot write binary file header");
  }

  CsvRecord book_record{};
  CsvRecord trade_record{};
  ReadStatus book_status = book.next(book_record);
  ReadStatus trade_status = trades ? trades->next(trade_record) : ReadStatus::End;
  if (book_status == ReadStatus::Error) {
    output.close();
    discard_temporary();
    return failure(book.error());
  }
  if (trade_status == ReadStatus::Error) {
    output.close();
    discard_temporary();
    return failure(trades->error());
  }

  bool have_pending = false;
  std::uint64_t pending_local_timestamp = 0;
  Event pending_event{};
  std::uint64_t event_count = 0;
  const auto emit = [&output, &event_count](const Event& event) {
    if (event_count == std::numeric_limits<std::uint64_t>::max()) return false;
    if (!write_event(output, event)) return false;
    ++event_count;
    return true;
  };

  while (book_status == ReadStatus::Row || trade_status == ReadStatus::Row) {
    const bool take_book =
        book_status == ReadStatus::Row &&
        (trade_status != ReadStatus::Row || book_record.timestamp_us <= trade_record.timestamp_us);
    const CsvRecord current = take_book ? book_record : trade_record;
    if (have_pending) {
      if (pending_local_timestamp != current.local_timestamp_us) {
        pending_event.flags |= kEventFlagEndOfMessage;
      }
      if (!emit(pending_event)) {
        output.close();
        discard_temporary();
        return failure("failed while writing binary event data");
      }
    }
    pending_event = current.event;
    pending_local_timestamp = current.local_timestamp_us;
    have_pending = true;

    if (take_book) book_status = book.next(book_record);
    else trade_status = trades->next(trade_record);
    if (book_status == ReadStatus::Error) {
      output.close();
      discard_temporary();
      return failure(book.error());
    }
    if (trade_status == ReadStatus::Error) {
      output.close();
      discard_temporary();
      return failure(trades->error());
    }
  }

  if (have_pending) {
    pending_event.flags |= kEventFlagEndOfMessage;
    if (!emit(pending_event)) {
      output.close();
      discard_temporary();
      return failure("failed while writing final binary event");
    }
  }

  const auto header =
      make_binary_file_header(config.price_scale, config.qty_scale, symbol, event_count);
  output.seekp(0);
  output.write(reinterpret_cast<const char*>(header.data()),
               static_cast<std::streamsize>(header.size()));
  output.flush();
  if (!output) {
    output.close();
    discard_temporary();
    return failure("failed while finalizing binary file header");
  }
  output.close();
  if (!output) {
    discard_temporary();
    return failure("failed while closing binary output");
  }

  std::error_code rename_error;
  std::filesystem::rename(temporary_file, config.output_file, rename_error);
  if (rename_error) {
    discard_temporary();
    return failure("cannot publish binary output: " + rename_error.message());
  }
  return CsvToBinaryResult{true, event_count, {}};
}

} // namespace tickbook
