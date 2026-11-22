#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <sys/resource.h>

#include "tickbook/binary_replay.hpp"
#include "tickbook/book_map.hpp"
#include "tickbook/books/flat_book.hpp"
#include "tickbook/books/offset_book.hpp"
#include "tickbook/latency_histogram.hpp"
#include "tickbook/replay.hpp"
#include "tickbook/ring_buffer.hpp"

namespace {

using namespace tickbook;

constexpr std::size_t kRingCapacity = 1024;
constexpr std::size_t kOverheadSamples = 10'000;
constexpr std::uint64_t kMaximumWindowTicks = 6'000'000;

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

enum class BookKind { Map, Flat, Offset };

[[nodiscard]] std::uint64_t timer_now_ns() noexcept {
  using Clock = std::chrono::steady_clock;
  static_assert(Clock::is_steady);
  const auto ticks = Clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(ticks).count());
}

[[nodiscard]] std::uint64_t timer_overhead_ns() noexcept {
  std::uint64_t total = 0;
  for (std::size_t i = 0; i < kOverheadSamples; ++i) {
    const auto start = timer_now_ns();
    const auto finish = timer_now_ns();
    total += finish - start;
  }
  return total / kOverheadSamples;
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
  output << "Usage: replay_latency FILE.bin [--book map|flat|offset] [--price-tick UNITS]\n";
}

struct PriceRange {
  Price minimum{std::numeric_limits<Price>::max()};
  Price maximum{std::numeric_limits<Price>::min()};
};

[[nodiscard]] bool scan_price_range(BinaryReplay& source, Price price_tick, PriceRange& range,
                                    std::size_t& window_size, std::string& error) {
  while (const auto view = source.next()) {
    if (view->type() == EventType::Trade || view->qty() < 0) continue;
    if (view->price() % price_tick != 0) {
      error = "event price is not aligned to --price-tick";
      return false;
    }
    range.minimum = std::min(range.minimum, view->price());
    range.maximum = std::max(range.maximum, view->price());
  }

  if (range.minimum <= range.maximum) {
    const auto distance =
        static_cast<std::uint64_t>(range.maximum) - static_cast<std::uint64_t>(range.minimum);
    const auto ticks = distance / static_cast<std::uint64_t>(price_tick);
    if (ticks >= kMaximumWindowTicks) {
      error = "observed price span exceeds the 6000000-tick offset-book safety limit";
      return false;
    }
    window_size = static_cast<std::size_t>(ticks + 1);
  }
  source.rewind();
  return true;
}

template <OrderBook Book>
void apply_event(Book& book, const Event& event) noexcept(noexcept(book.apply(event))) {
  if ((event.flags & kEventFlagSnapshotStart) != 0) book.clear();
  book.apply(event);
}

template <OrderBook Book> std::uint64_t warm_up(BinaryReplay& source, Book& book) noexcept {
  source.rewind();
  std::uint64_t count = 0;
  while (const auto view = source.next()) {
    apply_event(book, view->copy());
    ++count;
  }
  book.clear();
  source.rewind();
  return count;
}

template <OrderBook Book>
[[nodiscard]] bool measure_replay(BinaryReplay& source, Book& book, std::string_view selected_book,
                                  Price price_tick, std::string& error) {
  touch_stack_pages();
  const auto warmup_events = warm_up(source, book);
  SpscRing<Event, kRingCapacity> ring;
  LatencyHistogram histogram;
  histogram.reset();
  const auto overhead_ns = timer_overhead_ns();
  const auto expected_events = source.event_count();

#if defined(__linux__)
  replay_detail::ReplayAffinity affinity;
  if (!affinity.prepare(error) || !affinity.pin_consumer(error)) return false;
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
    affinity.restore_consumer();
#endif
    error = "cannot start replay latency producer: ";
    error += exception.what();
    return false;
  }

#if defined(__linux__)
  producer_affinity_error = affinity.pin_producer(producer.native_handle());
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
  Event event{};
  for (;;) {
    const auto start_ns = timer_now_ns();
    if (ring.try_pop(event)) {
      apply_event(book, event);
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
            << "allocation_tracking=after_startup_done\n"
            << "hot_path_allocations=" << measured_allocations << '\n'
            << "timer=steady_clock timer_overhead_ns=" << overhead_ns << '\n'
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

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    usage(std::cout);
    return 0;
  }
  if (argc != 2 && argc != 4 && argc != 6) {
    usage(std::cerr);
    return 2;
  }

  const std::filesystem::path path(argv[1]);
  BookKind kind = BookKind::Map;
  Price price_tick = 1;
  for (int i = 2; i < argc; i += 2) {
    const std::string_view option(argv[i]);
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
    } else {
      usage(std::cerr);
      return 2;
    }
  }

  BinaryReplay source;
  std::string error;
  if (!source.open(path, error)) {
    std::cerr << "replay_latency: " << error << '\n';
    return 1;
  }
  PriceRange range;
  std::size_t window_size = 1;
  if (!scan_price_range(source, price_tick, range, window_size, error)) {
    std::cerr << "replay_latency: " << error << '\n';
    return 1;
  }

  bool success = false;
  switch (kind) {
  case BookKind::Map: {
    BookMap book;
    success = measure_replay(source, book, book_name(kind), price_tick, error);
    break;
  }
  case BookKind::Flat: {
    FlatBook book;
    success = measure_replay(source, book, book_name(kind), price_tick, error);
    break;
  }
  case BookKind::Offset: {
    OffsetArrayBook book(window_size, price_tick);
    success = measure_replay(source, book, book_name(kind), price_tick, error);
    break;
  }
  }
  if (!success) {
    std::cerr << "replay_latency: " << error << '\n';
    return 1;
  }
  return 0;
}
