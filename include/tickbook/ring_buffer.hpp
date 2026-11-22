#pragma once
#include <array>
#include <atomic>
#include <cstddef>

namespace tickbook {

// 128 bytes covers Apple Silicon and x86; it avoids false sharing between the
// producer-owned and consumer-owned counters.
inline constexpr std::size_t kCacheLine = 128;

// Single-producer single-consumer lock-free queue. Capacity must be a power of two.
// Exactly one thread may call try_push and exactly one other may call try_pop.
template <typename T, std::size_t Capacity> class SpscRing {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of two");

public:
  [[nodiscard]] bool try_push(const T& value) noexcept {
    const std::size_t head = prod_.head.load(std::memory_order_relaxed);
    if (head - prod_.tail_cache >= Capacity) {
      prod_.tail_cache = cons_.tail.load(std::memory_order_acquire);
      if (head - prod_.tail_cache >= Capacity) return false; // full
    }
    buf_[head & kMask] = value;
    prod_.head.store(head + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool try_pop(T& out) noexcept {
    const std::size_t tail = cons_.tail.load(std::memory_order_relaxed);
    if (tail == cons_.head_cache) {
      cons_.head_cache = prod_.head.load(std::memory_order_acquire);
      if (tail == cons_.head_cache) return false; // empty
    }
    out = buf_[tail & kMask];
    cons_.tail.store(tail + 1, std::memory_order_release);
    return true;
  }

  static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
  static constexpr std::size_t kMask = Capacity - 1;

  struct alignas(kCacheLine) ProducerSide {
    std::atomic<std::size_t> head{0};
    std::size_t tail_cache{0}; // producer's last seen copy of the consumer tail
  };
  struct alignas(kCacheLine) ConsumerSide {
    std::atomic<std::size_t> tail{0};
    std::size_t head_cache{0}; // consumer's last seen copy of the producer head
  };

  ProducerSide prod_;
  ConsumerSide cons_;
  alignas(kCacheLine) std::array<T, Capacity> buf_{};
};

} // namespace tickbook
