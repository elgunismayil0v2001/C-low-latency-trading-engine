#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/resource.h>

#include "tickbook/latency_histogram.hpp"
#include "tickbook/nn/mlp.hpp"

namespace {

std::atomic<bool> allocation_tracking{};
std::atomic<std::uint64_t> allocation_count{};

void note_allocation() noexcept {
  if (allocation_tracking.load(std::memory_order_relaxed)) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
}

[[nodiscard]] std::uint64_t now_ns() noexcept {
  const auto time = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(time).count());
}

[[nodiscard]] std::uint64_t timer_resolution_ns() noexcept {
  auto previous = now_ns();
  std::uint64_t minimum_positive_delta = std::numeric_limits<std::uint64_t>::max();
  for (std::size_t sample = 0; sample < 100'000; ++sample) {
    const auto current = now_ns();
    if (current > previous)
      minimum_positive_delta = std::min(minimum_positive_delta, current - previous);
    previous = current;
  }
  return minimum_positive_delta == std::numeric_limits<std::uint64_t>::max()
             ? 0
             : minimum_positive_delta;
}

[[nodiscard]] std::uint64_t timer_overhead() noexcept {
  constexpr std::size_t kBatches = 101;
  constexpr std::size_t kPairsPerBatch = 64;
  std::array<std::uint64_t, kBatches> samples{};
  for (std::size_t index = 0; index < samples.size(); ++index) {
    std::uint64_t total = 0;
    for (std::size_t pair = 0; pair < kPairsPerBatch; ++pair) {
      const auto start = now_ns();
      const auto finish = now_ns();
      total += finish - start;
    }
    samples[index] = total / kPairsPerBatch;
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

[[nodiscard]] bool read_weights(const std::filesystem::path& path, std::vector<std::byte>& bytes) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  std::vector<char> chars((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  bytes.resize(chars.size());
  for (std::size_t index = 0; index < chars.size(); ++index) {
    bytes[index] = static_cast<std::byte>(static_cast<unsigned char>(chars[index]));
  }
  return true;
}

[[nodiscard]] bool parse_iterations(std::string_view value, std::uint64_t& iterations) noexcept {
  if (value.empty()) return false;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), iterations);
  return result.ec == std::errc{} && result.ptr == value.data() + value.size() && iterations > 0;
}

} // namespace

#define TICKBOOK_ALLOC_HOOK note_allocation
#include "counting_new_delete.hpp"

int main(int argc, char** argv) try {
  if (argc < 2) {
    std::cerr << "Usage: nn_latency WEIGHTS.bin [--model scalar|simd] [--iterations COUNT]\n";
    return 2;
  }
  std::uint64_t iterations = 1'000'000;
  bool use_simd = true;
  for (int argument = 2; argument < argc;) {
    const std::string_view option(argv[argument]);
    if (argument + 1 >= argc) {
      std::cerr << "nn_latency: each option requires a value\n";
      return 2;
    }
    const std::string_view value(argv[argument + 1]);
    if (option == "--model") {
      if (value == "scalar") use_simd = false;
      else if (value == "simd") use_simd = true;
      else {
        std::cerr << "nn_latency: model must be scalar or simd\n";
        return 2;
      }
    } else if (option == "--iterations") {
      if (!parse_iterations(value, iterations)) {
        std::cerr << "nn_latency: iteration count must be a positive integer\n";
        return 2;
      }
    } else {
      std::cerr << "nn_latency: unknown option\n";
      return 2;
    }
    argument += 2;
  }
  std::vector<std::byte> weight_bytes;
  if (!read_weights(argv[1], weight_bytes)) {
    std::cerr << "nn_latency: cannot read weights file\n";
    return 1;
  }
  tickbook::nn::MlpModel model{};
  const auto load_error = tickbook::nn::load_weights(weight_bytes, model);
  if (load_error != tickbook::nn::WeightsLoadError::None) {
    std::cerr << "nn_latency: " << tickbook::nn::weights_load_error_message(load_error) << '\n';
    return 1;
  }

  std::array<tickbook::FeatureVector, 256> inputs{};
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    const auto base = static_cast<std::int64_t>(index) * 1024;
    inputs[index] = {static_cast<std::int64_t>(index % 5),
                     static_cast<std::int32_t>(base - 100'000),
                     static_cast<std::int32_t>(100'000 - base),
                     base,
                     base * 2,
                     -base * 3,
                     base * 4,
                     -base * 5,
                     base * 6,
                     -base * 7,
                     base * 8};
  }
  for (const auto& input : inputs) {
    static_cast<void>(use_simd ? model.infer(input) : model.infer_scalar(input));
  }
  const auto overhead_ns = timer_overhead();
  const auto resolution_ns = timer_resolution_ns();
  tickbook::LatencyHistogram histogram;
  histogram.reset();
  std::uint32_t checksum = 0;
  rusage before{};
  rusage after{};
  if (::getrusage(RUSAGE_SELF, &before) != 0) {
    std::cerr << "nn_latency: cannot read process resource counters\n";
    return 1;
  }

  allocation_count.store(0, std::memory_order_relaxed);
  allocation_tracking.store(true, std::memory_order_relaxed);
  for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
    const auto start = now_ns();
    const auto& input = inputs[static_cast<std::size_t>(iteration) & 255U];
    const auto output = use_simd ? model.infer(input) : model.infer_scalar(input);
    const auto finish = now_ns();
    histogram.record(
        tickbook::latency_detail::subtract_timer_overhead(finish - start, overhead_ns));
    checksum ^= std::bit_cast<std::uint32_t>(output.values[iteration % output.count]);
  }
  allocation_tracking.store(false, std::memory_order_relaxed);
  const auto allocations = allocation_count.load(std::memory_order_relaxed);
  if (::getrusage(RUSAGE_SELF, &after) != 0) {
    std::cerr << "nn_latency: cannot read process resource counters\n";
    return 1;
  }
  const auto minor_faults = after.ru_minflt - before.ru_minflt;
  const auto p50 = histogram.percentile_upper_ns(5000);
  const auto p99 = histogram.percentile_upper_ns(9900);
  const auto p999 = histogram.percentile_upper_ns(9990);
  std::cout << "model=" << (use_simd ? "simd_mlp" : "scalar_mlp") << '\n'
            << "hidden_width=" << model.layers[0].output_width << '\n'
            << "calls=" << histogram.count() << '\n'
            << "timer=steady_clock timer_resolution_estimate_ns=" << resolution_ns
            << " timer_overhead_ns=" << overhead_ns << '\n'
            << "p50_ns=" << (p50 ? std::to_string(*p50) : "n/a") << '\n'
            << "p99_ns=" << (p99 ? std::to_string(*p99) : "n/a") << '\n'
            << "p99.9_ns=" << (p999 ? std::to_string(*p999) : "n/a") << '\n'
            << "max_ns=" << histogram.maximum_ns() << '\n'
            << "hot_path_allocations=" << allocations << '\n'
            << "minor_faults=" << minor_faults << '\n'
            << "checksum=" << checksum << '\n';
  if (allocations != 0) {
    std::cerr << "nn_latency: inference allocated after startup\n";
    return 1;
  }
#if defined(__linux__)
  if (minor_faults != 0) {
    std::cerr << "nn_latency: measured inference incurred minor page faults\n";
    return 1;
  }
#endif
  return 0;
} catch (const std::exception& error) {
  std::cerr << "nn_latency: " << error.what() << '\n';
  return 1;
}
