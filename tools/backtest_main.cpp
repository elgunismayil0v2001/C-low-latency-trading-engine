#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tickbook/binary_replay.hpp"
#include "tickbook/books/offset_book.hpp"
#include "tickbook/features/feature_engine.hpp"
#include "tickbook/fixed.hpp"
#include "tickbook/nn/mlp.hpp"
#include "tickbook/nn/signal.hpp"
#include "tickbook/sim/sim_exchange.hpp"
#include "tickbook/sim/strategy.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using tickbook::Price;
using tickbook::Qty;
constexpr std::int64_t kQ20 = 1'048'576;

enum class StrategyChoice : std::uint8_t { Threshold, Nn };

struct Options {
  std::string input;
  std::string weights;
  std::string order_size{"0.001"};
  std::string max_position{"0.01"};
  std::string price_tick{"0.01"};
  std::string threshold{"0.20"};
  std::string return_threshold{"1.0"};
  std::string direction_margin{"0.0"};
  std::uint64_t latency_ms{5};
  std::uint32_t fee_ppm{tickbook::sim::kDefaultTakerFeePpm};
  StrategyChoice strategy{StrategyChoice::Threshold};
};

void usage(std::ostream& output) {
  output << "Usage: backtest FILE.bin [--strategy threshold|nn] [--weights FILE] "
            "[--size QTY] [--max-position QTY] [--latency-ms N] [--fee-ppm N] "
            "[--price-tick PRICE] [--threshold IMBALANCE] "
            "[--return-threshold-ticks TICKS] [--direction-margin VALUE]\n";
}

[[nodiscard]] bool parse_unsigned(std::string_view text, std::uint64_t& output) noexcept {
  if (text.empty()) return false;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), output);
  return error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options) {
  if (argc < 2) return false;
  options.input = argv[1];
  for (int index = 2; index < argc; ++index) {
    const std::string_view name(argv[index]);
    if (name == "--help") return false;
    if (index + 1 >= argc) return false;
    const std::string_view value(argv[++index]);
    if (name == "--strategy") {
      if (value == "threshold") options.strategy = StrategyChoice::Threshold;
      else if (value == "nn") options.strategy = StrategyChoice::Nn;
      else return false;
    } else if (name == "--weights") options.weights = value;
    else if (name == "--size") options.order_size = value;
    else if (name == "--max-position") options.max_position = value;
    else if (name == "--price-tick") options.price_tick = value;
    else if (name == "--threshold") options.threshold = value;
    else if (name == "--return-threshold-ticks") options.return_threshold = value;
    else if (name == "--direction-margin") options.direction_margin = value;
    else if (name == "--latency-ms") {
      if (!parse_unsigned(value, options.latency_ms)) return false;
    } else if (name == "--fee-ppm") {
      std::uint64_t parsed = 0;
      if (!parse_unsigned(value, parsed) || parsed > 1'000'000) return false;
      options.fee_ppm = static_cast<std::uint32_t>(parsed);
    } else {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool decimal_to_q20(std::string_view text, std::int64_t& output) noexcept {
  std::int64_t micro = 0;
  if (!tickbook::parse_fixed(text, 6, micro)) return false;
  const auto value = static_cast<std::uint64_t>(micro);
  const auto whole = value / 1'000'000U;
  const auto remainder = value % 1'000'000U;
  const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (whole > maximum / static_cast<std::uint64_t>(kQ20)) return false;
  const auto scaled_whole = whole * static_cast<std::uint64_t>(kQ20);
  const auto scaled_fraction = remainder * static_cast<std::uint64_t>(kQ20) / 1'000'000U;
  if (scaled_fraction > maximum - scaled_whole) return false;
  output = static_cast<std::int64_t>(scaled_whole + scaled_fraction);
  return true;
}

[[nodiscard]] std::uint64_t timer_overhead_ns() noexcept {
  constexpr std::size_t kSamples = 10'000;
  std::uint64_t total = 0;
  for (std::size_t index = 0; index < kSamples; ++index) {
    const auto start = Clock::now();
    const auto end = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    if (elapsed > 0 &&
        static_cast<std::uint64_t>(elapsed) <= std::numeric_limits<std::uint64_t>::max() - total) {
      total += static_cast<std::uint64_t>(elapsed);
    }
  }
  return total / kSamples;
}

[[nodiscard]] bool load_model(const std::string& path, tickbook::nn::MlpModel& model,
                              std::string& error) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    error = "cannot open weights file: " + path;
    return false;
  }
  const auto end = input.tellg();
  if (end < 0 || static_cast<std::uint64_t>(end) > std::numeric_limits<std::size_t>::max()) {
    error = "weights file is too large";
    return false;
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(end));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!input) {
    error = "cannot read weights file: " + path;
    return false;
  }
  const auto result = tickbook::nn::load_weights(bytes, model);
  if (result != tickbook::nn::WeightsLoadError::None) {
    error = tickbook::nn::weights_load_error_message(result);
    return false;
  }
  return true;
}

[[nodiscard]] std::string fixed_unsigned_string(std::uint64_t magnitude, std::int32_t scale) {
  if (scale > 18) {
    std::string fractional = std::to_string(magnitude);
    if (fractional.size() < static_cast<std::size_t>(scale)) {
      fractional.insert(0, static_cast<std::size_t>(scale) - fractional.size(), '0');
    }
    return "0." + fractional;
  }
  std::uint64_t factor = 1;
  for (std::int32_t index = 0; index < scale; ++index) factor *= 10U;
  std::string result = std::to_string(magnitude / factor);
  if (scale != 0) {
    result += ".";
    auto fractional = std::to_string(magnitude % factor);
    result.append(static_cast<std::size_t>(scale) - fractional.size(), '0');
    result += fractional;
  }
  return result;
}

[[nodiscard]] std::string fixed_string(std::int64_t value, std::int32_t scale) {
  const auto negative = value < 0;
  const auto magnitude =
      negative ? static_cast<std::uint64_t>(-(value + 1)) + 1U : static_cast<std::uint64_t>(value);
  auto result = fixed_unsigned_string(magnitude, scale);
  if (negative) result.insert(result.begin(), '-');
  return result;
}

struct ScanResult {
  std::uint64_t events{};
  std::uint64_t messages{};
  Price minimum_price{std::numeric_limits<Price>::max()};
  Price maximum_price{};
  std::uint64_t book_events{};
};

[[nodiscard]] bool scan_range(tickbook::BinaryReplay& replay, Price price_tick, ScanResult& scan,
                              std::string& error) {
  replay.rewind();
  while (const auto view = replay.next()) {
    ++scan.events;
    if ((view->flags() & tickbook::kEventFlagEndOfMessage) != 0) ++scan.messages;
    if (view->type() != tickbook::EventType::Trade) {
      ++scan.book_events;
      if (view->price() < 0 || view->price() % price_tick != 0) {
        error = "negative or misaligned book price in replay file";
        return false;
      }
      scan.minimum_price = std::min(scan.minimum_price, view->price());
      scan.maximum_price = std::max(scan.maximum_price, view->price());
    }
  }
  replay.rewind();
  return true;
}

void print_latency(std::string_view name, const tickbook::sim::Metrics& metrics) {
  const auto& histogram = metrics.latency_histogram();
  std::cout << name << "_count=" << histogram.count() << '\n';
  if (const auto value = histogram.percentile_upper_ns(5'000))
    std::cout << name << "_p50_ns=" << *value << '\n';
  if (const auto value = histogram.percentile_upper_ns(9'900))
    std::cout << name << "_p99_ns=" << *value << '\n';
  if (const auto value = histogram.percentile_upper_ns(9'990))
    std::cout << name << "_p999_ns=" << *value << '\n';
  std::cout << name << "_max_ns=" << histogram.maximum_ns() << '\n';
}

} // namespace

int main(int argc, char** argv) try {
  Options options{};
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    usage(std::cout);
    return 0;
  }
  if (!parse_options(argc, argv, options)) {
    usage(std::cerr);
    return 2;
  }

  tickbook::BinaryReplay replay;
  std::string error;
  if (!replay.open(options.input, error)) {
    std::cerr << "backtest: " << error << '\n';
    return 1;
  }
  if (replay.price_scale() < 0 || replay.price_scale() > 18 || replay.qty_scale() < 0 ||
      replay.qty_scale() > 18) {
    std::cerr << "backtest: unsupported binary fixed-point scales\n";
    return 1;
  }

  Price price_tick = 0;
  if (!tickbook::parse_fixed(options.price_tick, replay.price_scale(), price_tick) ||
      price_tick <= 0) {
    std::cerr << "backtest: invalid --price-tick\n";
    return 2;
  }
  ScanResult scan{};
  if (!scan_range(replay, price_tick, scan, error)) {
    std::cerr << "backtest: " << error << '\n';
    return 1;
  }
  if (scan.book_events == 0) {
    std::cerr << "backtest: replay has no book events\n";
    return 1;
  }
  Qty order_size = 0;
  Qty max_position = 0;
  std::int64_t threshold_q20 = 0;
  std::int64_t return_threshold_q20 = 0;
  std::int64_t direction_margin_q20 = 0;
  if (!tickbook::parse_fixed(options.order_size, replay.qty_scale(), order_size) ||
      order_size <= 0 ||
      !tickbook::parse_fixed(options.max_position, replay.qty_scale(), max_position) ||
      max_position < order_size || !decimal_to_q20(options.threshold, threshold_q20) ||
      !decimal_to_q20(options.return_threshold, return_threshold_q20) ||
      !decimal_to_q20(options.direction_margin, direction_margin_q20)) {
    std::cerr << "backtest: invalid quantity, price tick, or signal threshold\n";
    return 2;
  }
  tickbook::nn::MlpModel model{};
  if (options.strategy == StrategyChoice::Nn) {
    if (options.weights.empty()) {
      std::cerr << "backtest: --strategy nn requires --weights\n";
      return 2;
    }
    if (!load_model(options.weights, model, error)) {
      std::cerr << "backtest: " << error << '\n';
      return 1;
    }
  }

  const auto price_span = static_cast<std::uint64_t>(scan.maximum_price) -
                          static_cast<std::uint64_t>(scan.minimum_price);
  const auto tick_count = price_span / static_cast<std::uint64_t>(price_tick) + 1U;
  if (tick_count > std::numeric_limits<std::size_t>::max()) {
    std::cerr << "backtest: observed price range is too large\n";
    return 1;
  }
  tickbook::OffsetArrayBook book(static_cast<std::size_t>(tick_count), price_tick);
  if (book.window_size() == 0) {
    std::cerr << "backtest: cannot allocate a book window for this price range\n";
    return 1;
  }
  tickbook::FeatureEngine features({price_tick, tickbook::kDefaultFeatureTimeCapacity});
  if (options.latency_ms >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 1'000'000)) {
    std::cerr << "backtest: latency is too large\n";
    return 2;
  }
  tickbook::sim::SimExchange exchange({static_cast<std::int64_t>(options.latency_ms * 1'000'000U),
                                       options.fee_ppm, replay.qty_scale(), order_size,
                                       max_position});
  if (!exchange.valid()) {
    std::cerr << "backtest: invalid simulated exchange configuration\n";
    return 2;
  }
  const tickbook::sim::StrategyLimits limits{order_size, max_position};
  tickbook::sim::ThresholdStrategy threshold_strategy(limits, threshold_q20);
  tickbook::sim::NnStrategy nn_strategy(limits, return_threshold_q20, direction_margin_q20);
  tickbook::sim::IStrategy& strategy =
      options.strategy == StrategyChoice::Nn
          ? static_cast<tickbook::sim::IStrategy&>(nn_strategy)
          : static_cast<tickbook::sim::IStrategy&>(threshold_strategy);

  const auto overhead_ns = timer_overhead_ns();
  const auto run = [&](bool measure) noexcept -> bool {
    book.clear();
    features.reset();
    exchange.reset();
    replay.rewind();
    while (const auto view = replay.next()) {
      const auto start = measure ? Clock::now() : Clock::time_point{};
      const auto event = view->copy();
      if ((event.flags & tickbook::kEventFlagSnapshotStart) != 0) book.clear();
      book.apply(event);
      const auto update = features.on_event(event, book);
      if (update == tickbook::FeatureUpdateResult::InvalidConfiguration ||
          update == tickbook::FeatureUpdateResult::InvalidTimestamp ||
          update == tickbook::FeatureUpdateResult::TimeWindowCapacityExceeded) {
        return false;
      }
      if (update == tickbook::FeatureUpdateResult::Updated) {
        if (exchange.process_due(event.ts_ns, book) != tickbook::sim::ExchangeStatus::Ok) {
          return false;
        }
        const auto& latest = features.latest();
        tickbook::sim::BookView book_view{book.best_bid(), book.best_ask(), exchange.position(),
                                          exchange.projected_position()};
        const auto signal = options.strategy == StrategyChoice::Nn
                                ? tickbook::nn::make_signal(model, latest.values)
                                : tickbook::sim::imbalance_signal(latest.values);
        const auto intent = strategy.on_signal(signal, book_view);
        if (intent) {
          (void)exchange.submit(intent, event.ts_ns);
          if (exchange.latency_ns() == 0 &&
              exchange.process_due(event.ts_ns, book) != tickbook::sim::ExchangeStatus::Ok) {
            return false;
          }
        }
        exchange.mark_to_market(book);
      }
      if (measure) {
        const auto end = Clock::now();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        const auto nonnegative = elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0U;
        const auto adjusted =
            tickbook::latency_detail::subtract_timer_overhead(nonnegative, overhead_ns);
        exchange.record_latency(adjusted);
      }
    }
    exchange.cancel_pending();
    exchange.mark_to_market(book);
    return true;
  };

  if (!run(false)) {
    std::cerr << "backtest: replay failed during warm-up" << '\n';
    return 1;
  }
  if (!run(true)) {
    std::cerr << "backtest: replay failed during measured pass" << '\n';
    return 1;
  }

  const auto result = exchange.metrics().snapshot();
  std::cout << "symbol=";
  for (const char character : replay.symbol()) {
    if (character == '\0') break;
    std::cout << character;
  }
  std::cout << '\n'
            << "events=" << scan.events << '\n'
            << "messages=" << scan.messages << '\n'
            << "price_scale=" << replay.price_scale() << '\n'
            << "qty_scale=" << replay.qty_scale() << '\n'
            << "strategy=" << (options.strategy == StrategyChoice::Nn ? "nn" : "threshold") << '\n'
            << "order_latency_ms=" << options.latency_ms << '\n'
            << "fee_ppm=" << options.fee_ppm << '\n'
            << "fee_bps=" << options.fee_ppm / 100U << '\n'
            << "quote_ledger_extra_decimals=6\n"
            << "gross_pnl_quote=" << fixed_string(result.gross_pnl, replay.price_scale() + 6)
            << '\n'
            << "net_pnl_quote=" << fixed_string(result.net_pnl, replay.price_scale() + 6) << '\n'
            << "fees_quote=" << fixed_string(result.fees, replay.price_scale() + 6) << '\n'
            << "turnover_quote=" << fixed_string(result.turnover, replay.price_scale() + 6) << '\n'
            << "max_drawdown_quote="
            << fixed_string(result.maximum_drawdown, replay.price_scale() + 6) << '\n'
            << "closed_trades=" << result.closed_trades << '\n'
            << "average_trade_pnl_bps=" << result.average_trade_pnl_bps << '\n'
            << "hit_rate_bps=" << result.hit_rate_bps << '\n'
            << "break_even_fee_bps=" << result.break_even_fee_bps << '\n'
            << "orders_submitted=" << result.orders_submitted << '\n'
            << "orders_accepted=" << result.orders_accepted << '\n'
            << "orders_rejected=" << result.orders_rejected << '\n'
            << "orders_filled=" << result.orders_filled << '\n'
            << "fills=" << result.fills << '\n'
            << "unfilled_qty=" << fixed_unsigned_string(result.unfilled_qty, replay.qty_scale())
            << '\n'
            << "final_position=" << fixed_string(exchange.position(), replay.qty_scale()) << '\n'
            << "overflowed=" << (result.overflowed || exchange.overflowed() ? "true" : "false")
            << '\n'
            << "timer_overhead_ns=" << overhead_ns << '\n';
  print_latency("engine_latency", exchange.metrics());
  return 0;
} catch (const std::exception& exception) {
  std::cerr << "backtest: " << exception.what() << '\n';
  return 1;
}
