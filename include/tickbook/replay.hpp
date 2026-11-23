#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

#include "tickbook/binary_replay.hpp"
#include "tickbook/book_map.hpp"
#include "tickbook/ring_buffer.hpp"

#if defined(__linux__)
#include <cerrno>
#include <pthread.h>
#include <sched.h>
#endif

namespace tickbook {

inline constexpr std::size_t kReplayRingCapacity = 1024;

struct ReplaySummary {
  bool success{};
  std::string error;
  std::array<char, 16> symbol{};
  std::int32_t price_scale{};
  std::int32_t qty_scale{};
  std::uint64_t event_count{};
  std::size_t bid_levels{};
  std::size_t ask_levels{};
  std::optional<Level> best_bid;
  std::optional<Level> best_ask;
  std::uint64_t book_checksum{};
};

struct ReplayOptions {
  bool lock_pages{};
  bool pin_threads{};
};

namespace replay_detail {

#if defined(__linux__)
class ReplayAffinity {
public:
  bool prepare(std::string& error) {
    if (::sched_getaffinity(0, sizeof(original_), &original_) != 0) {
      error = system_error("cannot read replay CPU affinity", errno);
      return false;
    }

    struct Core {
      int cpu;
      int package;
      int core;
    };
    std::array<Core, CPU_SETSIZE> cores{};
    std::size_t count = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
      if (!CPU_ISSET(cpu, &original_)) continue;
      int package = 0;
      int core = 0;
      if (read_topology(cpu, "physical_package_id", package) &&
          read_topology(cpu, "core_id", core)) {
        cores[count++] = Core{cpu, package, core};
      }
    }

    for (std::size_t i = 0; i < count; ++i) {
      for (std::size_t j = i + 1; j < count; ++j) {
        if (cores[i].package != cores[j].package || cores[i].core != cores[j].core) {
          consumer_cpu_ = cores[i].cpu;
          producer_cpu_ = cores[j].cpu;
          return true;
        }
      }
    }
    error = "replay requires two available physical CPU cores on Linux";
    return false;
  }

  bool pin_consumer(std::string& error) {
    const int status = pin_thread(::pthread_self(), consumer_cpu_);
    if (status != 0) {
      error = system_error("cannot pin replay consumer", status);
      return false;
    }
    consumer_pinned_ = true;
    return true;
  }

  [[nodiscard]] int pin_producer(pthread_t thread) const noexcept {
    return pin_thread(thread, producer_cpu_);
  }

  void restore_consumer() noexcept {
    if (consumer_pinned_) {
      ::pthread_setaffinity_np(::pthread_self(), sizeof(original_), &original_);
      consumer_pinned_ = false;
    }
  }

  [[nodiscard]] static std::string system_error(std::string_view operation, int error_code) {
    return std::string(operation) + ": " +
           std::error_code(error_code, std::generic_category()).message();
  }

private:
  static bool read_topology(int cpu, std::string_view field, int& value) {
    const std::string path =
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/" + std::string(field);
    std::ifstream input(path);
    return static_cast<bool>(input >> value);
  }

  static int pin_thread(pthread_t thread, int cpu) noexcept {
    cpu_set_t selected;
    CPU_ZERO(&selected);
    CPU_SET(cpu, &selected);
    return ::pthread_setaffinity_np(thread, sizeof(selected), &selected);
  }

  cpu_set_t original_{};
  int consumer_cpu_{-1};
  int producer_cpu_{-1};
  bool consumer_pinned_{};
};
#endif

inline void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
  hash ^= value;
  hash *= 1099511628211ULL;
}

inline void hash_u64(std::uint64_t& hash, std::uint64_t value) noexcept {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    hash_byte(hash, static_cast<std::uint8_t>((value >> (8U * i)) & 0xffU));
  }
}

inline std::uint64_t book_checksum(const BookMap& book) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  hash_u64(hash, static_cast<std::uint64_t>(book.bid_levels()));
  book.visit_bids([&hash](Price price, Qty qty) noexcept {
    hash_byte(hash, static_cast<std::uint8_t>(Side::Bid));
    hash_u64(hash, std::bit_cast<std::uint64_t>(price));
    hash_u64(hash, std::bit_cast<std::uint64_t>(qty));
  });
  hash_u64(hash, static_cast<std::uint64_t>(book.ask_levels()));
  book.visit_asks([&hash](Price price, Qty qty) noexcept {
    hash_byte(hash, static_cast<std::uint8_t>(Side::Ask));
    hash_u64(hash, std::bit_cast<std::uint64_t>(price));
    hash_u64(hash, std::bit_cast<std::uint64_t>(qty));
  });
  return hash;
}

} // namespace replay_detail

template <std::size_t RingCapacity = kReplayRingCapacity>
[[nodiscard]] ReplaySummary run_binary_replay(const std::filesystem::path& path,
                                              ReplayOptions options = {}) {
  ReplaySummary result{};
  BinaryReplay source;
  if (!source.open(path, result.error, options.lock_pages)) return result;

  result.symbol = source.symbol();
  result.price_scale = source.price_scale();
  result.qty_scale = source.qty_scale();

#if defined(__linux__)
  replay_detail::ReplayAffinity affinity;
  bool consumer_pinned = false;
  if (options.pin_threads) {
    if (!affinity.prepare(result.error) || !affinity.pin_consumer(result.error)) return result;
    consumer_pinned = true;
  }
#endif

  SpscRing<Event, RingCapacity> ring;
  std::atomic<bool> producer_done{false};
#if defined(__linux__)
  std::atomic<bool> producer_start{false};
  int producer_affinity_error = 0;
#endif
  std::uint64_t produced = 0;
  std::thread producer;
  try {
#if defined(__linux__)
    producer = std::thread(
        [&source, &ring, &producer_done, &produced, &producer_start, &producer_affinity_error]() {
#else
    producer = std::thread([&source, &ring, &producer_done, &produced]() {
#endif
#if defined(__linux__)
          // The acquire pairs with the startup release so the affinity result is visible.
          while (!producer_start.load(std::memory_order_acquire)) std::this_thread::yield();
          if (producer_affinity_error != 0) {
            producer_done.store(true, std::memory_order_release);
            return;
          }
#endif
          while (const auto view = source.next()) {
            const Event event = view->copy();
            while (!ring.try_push(event)) std::this_thread::yield();
            ++produced;
          }
          // The release pairs with the consumer acquire so it cannot finish before the
          // producer's final queue push has completed.
          producer_done.store(true, std::memory_order_release);
        });
  } catch (const std::system_error& exception) {
#if defined(__linux__)
    affinity.restore_consumer();
#endif
    result.error = "cannot start replay producer: ";
    result.error += exception.what();
    return result;
  }

#if defined(__linux__)
  if (options.pin_threads) {
    producer_affinity_error = affinity.pin_producer(producer.native_handle());
  }
  // This release starts the producer after its affinity result is visible to it.
  producer_start.store(true, std::memory_order_release);
#endif

  BookMap book;
  Event event{};
  std::uint64_t consumed = 0;
  const auto apply = [&book, &consumed](const Event& next) {
    if ((next.flags & kEventFlagSnapshotStart) != 0) book.clear();
    book.apply(next);
    ++consumed;
  };
  for (;;) {
    if (ring.try_pop(event)) {
      apply(event);
      continue;
    }
    // Acquire observes producer_done's release and all queue pushes before it.
    if (producer_done.load(std::memory_order_acquire)) {
      if (!ring.try_pop(event)) break;
      apply(event);
      continue;
    }
    std::this_thread::yield();
  }
  producer.join();

#if defined(__linux__)
  if (consumer_pinned) affinity.restore_consumer();
  if (producer_affinity_error != 0) {
    result.error = replay_detail::ReplayAffinity::system_error("cannot pin replay producer",
                                                               producer_affinity_error);
    return result;
  }
#endif

  if (produced != consumed || consumed != source.event_count()) {
    result.error = "replay event counts do not match the binary file";
    return result;
  }

  result.success = true;
  result.event_count = consumed;
  result.bid_levels = book.bid_levels();
  result.ask_levels = book.ask_levels();
  result.best_bid = book.best_bid();
  result.best_ask = book.best_ask();
  result.book_checksum = replay_detail::book_checksum(book);
  return result;
}

} // namespace tickbook
