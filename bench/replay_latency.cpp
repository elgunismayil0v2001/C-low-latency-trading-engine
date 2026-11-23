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
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <sys/resource.h>

#include "tickbook/binary_replay.hpp"
#include "tickbook/book_map.hpp"
#include "tickbook/books/flat_book.hpp"
#include "tickbook/books/offset_book.hpp"
#include "tickbook/features/feature_engine.hpp"
#include "tickbook/latency_histogram.hpp"
#include "tickbook/nn/mlp.hpp"
#include "tickbook/replay.hpp"
#include "tickbook/ring_buffer.hpp"

namespace {

using namespace tickbook;

constexpr std::size_t kRingCapacity = 1024;

std::atomic<bool> allocation_tracking_enabled{};
std::atomic<std::size_t> allocation_count{};

void record_allocation() noexcept {
  // Allocations are counted only after replay startup has completed.
  if (allocation_tracking_enabled.load(std::memory_order_relaxed)) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
}

class AllocationTracking {
public:
  AllocationTracking() noexcept {
    allocation_count.store(0, std::memory_order_relaxed);
    allocation_tracking_enabled.store(true, std::memory_order_relaxed);
  }

  ~AllocationTracking() { static_cast<void>(stop()); }

  AllocationTracking(const AllocationTracking&) = delete;
  AllocationTracking& operator=(const AllocationTracking&) = delete;

  [[nodiscard]] std::size_t stop() noexcept {
    if (active_) {
      allocation_tracking_enabled.store(false, std::memory_order_relaxed);
      active_ = false;
    }
    return allocation_count.load(std::memory_order_relaxed);
  }

private:
  bool active_{true};
};

enum class BookKind : std::uint8_t { Map, Flat, Offset };

[[nodiscard]] std::uint64_t timer_now_ns() noexcept {
  using Clock = std::chrono::steady_clock;
  static_assert(Clock::is_steady);
  const auto ticks = Clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(ticks).count());
}

[[nodiscard]] std::uint64_t timer_resolution_ns() noexcept {
  auto previous = timer_now_ns();
  std::uint64_t minimum_positive_delta = std::numeric_limits<std::uint64_t>::max();
  for (std::size_t sample = 0; sample < 100'000; ++sample) {
    const auto current = timer_now_ns();
    if (current > previous) {
      minimum_positive_delta = std::min(minimum_positive_delta, current - previous);
    }
    previous = current;
  }
  return minimum_positive_delta == std::numeric_limits<std::uint64_t>::max()
             ? 0
             : minimum_positive_delta;
}

[[nodiscard]] std::uint64_t timer_overhead_ns() noexcept {
  constexpr std::size_t kBatches = 101;
  constexpr std::size_t kPairsPerBatch = 64;
  std::array<std::uint64_t, kBatches> samples{};
  for (std::size_t i = 0; i < samples.size(); ++i) {
    std::uint64_t total = 0;
    for (std::size_t pair = 0; pair < kPairsPerBatch; ++pair) {
      const auto start = timer_now_ns();
      const auto finish = timer_now_ns();
      total += finish - start;
    }
    samples[i] = total / kPairsPerBatch;
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
  __asm__ __volatile__("yield" ::: "memory");
#else
  // The compiler fence keeps the fallback spin from being optimized away.
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

void touch_stack_pages() noexcept {
  volatile unsigned char pages[256 * 1024]{};
  for (std::size_t index = 0; index < sizeof(pages); index += 4096) pages[index] = 0;
}

[[nodiscard]] std::string_view book_name(BookKind kind) noexcept {
  switch (kind) {
  case BookKind::Map: return "map";
  case BookKind::Flat: return "flat";
  case BookKind::Offset: return "offset";
  }
  return "unknown";
}

[[nodiscard]] bool parse_unsigned(std::string_view text, std::uint64_t& value) noexcept {
  if (text.empty()) return false;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_price_tick(std::string_view text, Price& value) noexcept {
  std::uint64_t parsed = 0;
  if (!parse_unsigned(text, parsed) || parsed == 0 ||
      parsed > static_cast<std::uint64_t>(std::numeric_limits<Price>::max())) {
    return false;
  }
  value = static_cast<Price>(parsed);
  return true;
}

void usage(std::ostream& output) {
  output << "Usage: replay_latency FILE.bin [--book map|flat|offset] [--price-tick UNITS] "
            "[--features] [--weights WEIGHTS.bin] [--lock-pages] [--pin-threads]\n";
}

[[nodiscard]] bool load_weights_file(const std::filesystem::path& path, nn::MlpModel& model,
                                     std::string& error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "cannot open weights file '" + path.string() + "'";
    return false;
  }
  std::vector<char> chars((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes(chars.size());
  for (std::size_t index = 0; index < chars.size(); ++index) {
    bytes[index] = static_cast<std::byte>(static_cast<unsigned char>(chars[index]));
  }
  const auto load_error = nn::load_weights(bytes, model);
  if (load_error != nn::WeightsLoadError::None) {
    error = nn::weights_load_error_message(load_error);
    return false;
  }
  return true;
}

template <OrderBook Book>
void apply_event(Book& book, const Event& event) noexcept(noexcept(book.apply(event))) {
  if ((event.flags & kEventFlagSnapshotStart) != 0) book.clear();
  book.apply(event);
}

template <OrderBook Book>
std::uint64_t warm_up(BinaryReplay& source, Book& book, FeatureEngine* features,
                      const nn::MlpModel* model, std::uint32_t& checksum) {
  source.rewind();
  std::uint64_t count = 0;
  while (const auto view = source.next()) {
    const auto event = view->copy();
    apply_event(book, event);
    if (features != nullptr) {
      const auto update = features->on_event(event, book);
      if (model != nullptr && update == FeatureUpdateResult::Updated) {
        const auto signal = model->infer(features->latest().values);
        for (std::size_t index = 0; index < signal.count; ++index) {
          checksum ^= std::bit_cast<std::uint32_t>(signal.values[index]);
        }
      }
    }
    ++count;
  }
  book.clear();
  if (features != nullptr) features->reset();
  source.rewind();
  return count;
}

template <OrderBook Book>
[[nodiscard]] bool measure_replay(BinaryReplay& source, Book& book, std::string_view selected_book,
                                  Price price_tick, bool enable_features, const nn::MlpModel* model,
                                  bool pin_threads, std::string& error) {
#if !defined(__linux__)
  static_cast<void>(pin_threads);
#endif
  touch_stack_pages();
  std::optional<FeatureEngine> feature_engine;
  if (enable_features)
    feature_engine.emplace(FeatureEngineConfig{price_tick, kDefaultFeatureTimeCapacity});
  std::uint32_t signal_checksum = 0;
  const auto warmup_events =
      warm_up(source, book, feature_engine ? &*feature_engine : nullptr, model, signal_checksum);
  SpscRing<Event, kRingCapacity> ring;
  LatencyHistogram histogram;
  histogram.reset();
  const auto overhead_ns = timer_overhead_ns();
  const auto resolution_ns = timer_resolution_ns();
  const auto expected_events = source.event_count();

#if defined(__linux__)
  replay_detail::ReplayAffinity affinity;
  bool consumer_pinned = false;
  if (pin_threads) {
    if (!affinity.prepare(error) || !affinity.pin_consumer(error)) return false;
    consumer_pinned = true;
  }
#endif

  std::atomic<bool> producer_start{false};
  std::atomic<bool> producer_ready{false};
  std::atomic<bool> producer_done{false};
  std::uint64_t produced = 0;
#if defined(__linux__)
  int producer_affinity_error = 0;
#endif

  std::thread producer;
  try {
    producer = std::thread([&]() noexcept {
      touch_stack_pages();
      // The acquire pairs with main's release, publishing affinity status and setup.
      while (!producer_start.load(std::memory_order_acquire)) cpu_pause();
#if defined(__linux__)
      if (producer_affinity_error != 0) {
        producer_done.store(true, std::memory_order_release);
        return;
      }
#endif

      const auto enqueue_next = [&]() noexcept {
        const auto view = source.next();
        if (!view) return false;
        const Event event = view->copy();
        while (!ring.try_push(event)) cpu_pause();
        ++produced;
        return true;
      };

      const auto prefill_count = std::min<std::uint64_t>(expected_events, kRingCapacity);
      for (std::uint64_t i = 0; i < prefill_count; ++i) {
        if (!enqueue_next()) break;
      }
      if (prefill_count == kRingCapacity && expected_events > kRingCapacity) {
        const Event probe{};
        static_cast<void>(ring.try_push(probe));
        cpu_pause();
      }
      // The release publishes the prefetched events before the consumer starts timing.
      producer_ready.store(true, std::memory_order_release);
      while (enqueue_next()) {}
      // The release follows the final queue push; consumer acquire sees all work.
      producer_done.store(true, std::memory_order_release);
    });
  } catch (const std::system_error& exception) {
#if defined(__linux__)
    if (consumer_pinned) affinity.restore_consumer();
#endif
    error = "cannot start replay latency producer: ";
    error += exception.what();
    return false;
  }

#if defined(__linux__)
  if (pin_threads) producer_affinity_error = affinity.pin_producer(producer.native_handle());
#endif
  // Startup includes the warm-up, all preallocation, and thread creation. Track every
  // allocation from the first replay event through producer and consumer completion.
  AllocationTracking allocation_tracking;
  // The release starts the producer after its affinity result has been written.
  producer_start.store(true, std::memory_order_release);
#if defined(__linux__)
  if (producer_affinity_error != 0) {
    producer.join();
    affinity.restore_consumer();
    error = replay_detail::ReplayAffinity::system_error("cannot pin replay producer",
                                                        producer_affinity_error);
    return false;
  }
#endif

  // The acquire observes the producer's release after it filled the ring before timing.
  while (!producer_ready.load(std::memory_order_acquire)) cpu_pause();

  rusage before{};
  rusage after{};
  constexpr int kUsageWho = RUSAGE_SELF;
  const bool got_before = ::getrusage(kUsageWho, &before) == 0;

  std::uint64_t consumed = 0;
  std::uint64_t feature_messages = 0;
  std::uint64_t signal_messages = 0;
  bool feature_update_failed = false;
  Event event{};
  for (;;) {
    const auto start_ns = timer_now_ns();
    if (ring.try_pop(event)) {
      apply_event(book, event);
      if (feature_engine) {
        const auto update = feature_engine->on_event(event, book);
        if (update == FeatureUpdateResult::Updated) {
          ++feature_messages;
          if (model != nullptr) {
            const auto signal = model->infer(feature_engine->latest().values);
            for (std::size_t index = 0; index < signal.count; ++index) {
              signal_checksum ^= std::bit_cast<std::uint32_t>(signal.values[index]);
            }
            ++signal_messages;
          }
        }
        if (update == FeatureUpdateResult::InvalidConfiguration ||
            update == FeatureUpdateResult::InvalidTimestamp ||
            update == FeatureUpdateResult::TimeWindowCapacityExceeded) {
          feature_update_failed = true;
        }
      }
      const auto finish_ns = timer_now_ns();
      histogram.record(latency_detail::subtract_timer_overhead(finish_ns - start_ns, overhead_ns));
      ++consumed;
      continue;
    }
    // Acquire observes producer_done's release and its final queue push.
    if (producer_done.load(std::memory_order_acquire)) break;
    cpu_pause();
  }
  const bool got_after = ::getrusage(kUsageWho, &after) == 0;
  producer.join();
  const auto measured_allocations = allocation_tracking.stop();

#if defined(__linux__)
  affinity.restore_consumer();
#endif

  if (produced != expected_events || consumed != expected_events) {
    error = "replay latency event counts do not match the binary file";
    return false;
  }
  if (feature_update_failed) {
    error = "feature engine rejected the replay event stream";
    return false;
  }
  if (!got_before || !got_after) {
    error = "cannot read minor page-fault counters around measured replay";
    return false;
  }
  if (selected_book == "map" && measured_allocations == 0) {
    error = "BookMap allocation positive control did not allocate";
    return false;
  }
  if (selected_book != "map" && measured_allocations != 0) {
    error = "hot-path allocation count must be zero after startup";
    return false;
  }
  const auto minor_faults = after.ru_minflt - before.ru_minflt;

  const auto p50 = histogram.percentile_upper_ns(5000);
  const auto p99 = histogram.percentile_upper_ns(9900);
  const auto p999 = histogram.percentile_upper_ns(9990);
  std::cout << "book=" << selected_book << '\n'
            << "price_tick_fixed_point_units=" << price_tick << '\n'
            << "events=" << histogram.count() << '\n'
            << "warmup_events=" << warmup_events << '\n'
            << "features=" << (feature_engine ? "enabled" : "disabled") << '\n'
            << "feature_messages=" << feature_messages << '\n'
            << "nn_inference=" << (model != nullptr ? "enabled" : "disabled") << '\n'
            << "nn_messages=" << signal_messages << '\n'
            << "signal_checksum=" << signal_checksum << '\n'
            << "allocation_tracking=after_startup_done\n"
            << "hot_path_allocations=" << measured_allocations << '\n'
            << "ring_hop=included\n"
            << "timer=steady_clock timer_resolution_estimate_ns=" << resolution_ns
            << " timer_overhead_ns=" << overhead_ns << '\n'
            << "p50_ns=" << (p50 ? std::to_string(*p50) : "n/a") << '\n'
            << "p99_ns=" << (p99 ? std::to_string(*p99) : "n/a") << '\n'
            << "p99.9_ns=" << (p999 ? std::to_string(*p999) : "n/a") << '\n'
            << "max_ns=" << histogram.maximum_ns() << '\n'
            << "minor_faults_before=" << before.ru_minflt << '\n'
            << "minor_faults_after=" << after.ru_minflt << '\n'
            << "minor_faults=" << minor_faults << '\n';
#if defined(__linux__)
  std::cout << "minor_fault_assertion=zero_required\n";
  if (minor_faults != 0) {
    error = "measured replay incurred minor page faults";
    return false;
  }
#else
  std::cout << "minor_fault_assertion=report_only_process_scope\n";
#endif
  return true;
}

} // namespace

void* operator new(std::size_t size) {
  if (void* memory = std::malloc(size == 0 ? 1 : size)) {
    record_allocation();
    return memory;
  }
  throw std::bad_alloc{};
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

void* operator new(std::size_t size, std::align_val_t alignment) {
  void* memory = nullptr;
  const auto alignment_size = static_cast<std::size_t>(alignment);
  if (posix_memalign(&memory, alignment_size, size == 0 ? alignment_size : size) != 0) {
    throw std::bad_alloc{};
  }
  record_allocation();
  return memory;
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}

void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }

int main(int argc, char** argv) try {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    usage(std::cout);
    return 0;
  }
  if (argc < 2) {
    usage(std::cerr);
    return 2;
  }

  const std::filesystem::path path(argv[1]);
  BookKind kind = BookKind::Map;
  Price price_tick = 1;
  bool enable_features = false;
  bool lock_pages = false;
  bool pin_threads = false;
  std::filesystem::path weights_path;
  for (int i = 2; i < argc;) {
    const std::string_view option(argv[i]);
    if (option == "--features") {
      enable_features = true;
      ++i;
      continue;
    }
    if (option == "--lock-pages") {
      lock_pages = true;
      ++i;
      continue;
    }
    if (option == "--pin-threads") {
      pin_threads = true;
      ++i;
      continue;
    }
    if (i + 1 >= argc) {
      usage(std::cerr);
      return 2;
    }
    const std::string_view value(argv[i + 1]);
    if (option == "--book") {
      if (value == "map") kind = BookKind::Map;
      else if (value == "flat") kind = BookKind::Flat;
      else if (value == "offset") kind = BookKind::Offset;
      else {
        std::cerr << "replay_latency: book must be map, flat, or offset\n";
        return 2;
      }
    } else if (option == "--price-tick") {
      if (!parse_price_tick(value, price_tick)) {
        std::cerr << "replay_latency: price tick must be a positive integer\n";
        return 2;
      }
    } else if (option == "--weights") {
      weights_path = value;
      enable_features = true;
    } else {
      usage(std::cerr);
      return 2;
    }
    i += 2;
  }

  BinaryReplay source;
  std::string error;
  if (!source.open(path, error, lock_pages)) {
    std::cerr << "replay_latency: " << error << '\n';
    return 1;
  }
  std::optional<nn::MlpModel> model;
  if (!weights_path.empty()) {
    model.emplace();
    if (!load_weights_file(weights_path, *model, error)) {
      std::cerr << "replay_latency: " << error << '\n';
      return 1;
    }
  }
  bool success = false;
  switch (kind) {
  case BookKind::Map: {
    BookMap book;
    success = measure_replay(source, book, book_name(kind), price_tick, enable_features,
                             model ? &*model : nullptr, pin_threads, error);
    break;
  }
  case BookKind::Flat: {
    FlatBook book;
    success = measure_replay(source, book, book_name(kind), price_tick, enable_features,
                             model ? &*model : nullptr, pin_threads, error);
    break;
  }
  case BookKind::Offset: {
    OffsetArrayBook book(kDefaultOffsetArrayWindowSize, price_tick);
    success = measure_replay(source, book, book_name(kind), price_tick, enable_features,
                             model ? &*model : nullptr, pin_threads, error);
    break;
  }
  }
  if (!success) {
    std::cerr << "replay_latency: " << error << '\n';
    return 1;
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << "replay_latency: " << error.what() << '\n';
  return 1;
}
