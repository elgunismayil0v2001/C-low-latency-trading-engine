#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include "tickbook/binary_file.hpp"
#include "tickbook/binary_replay.hpp"
#include "tickbook/book_map.hpp"
#include "tickbook/books/flat_book.hpp"
#include "tickbook/books/offset_book.hpp"
#include "tickbook/ring_buffer.hpp"
#include "tickbook/types.hpp"

namespace {

thread_local bool count_this_thread = false;
std::atomic<std::size_t> allocation_count{};

void record_allocation() noexcept {
  if (count_this_thread) {
    // Relaxed is enough because join precedes reading; this only combines allocation totals.
    allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
}

class CountAllocations {
public:
  CountAllocations() noexcept : previous_(count_this_thread) { count_this_thread = true; }
  ~CountAllocations() { count_this_thread = previous_; }

  CountAllocations(const CountAllocations&) = delete;
  CountAllocations& operator=(const CountAllocations&) = delete;

private:
  bool previous_;
};

constexpr std::size_t kFixtureEventCount = 512;
constexpr std::size_t kRingCapacity = 8;

void write_u64_le(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) noexcept {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    bytes[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xffU);
  }
}

std::array<std::byte, tickbook::kBinaryEventSize>
encode_event(const tickbook::Event& event) noexcept {
  std::array<std::byte, tickbook::kBinaryEventSize> bytes{};
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

std::array<tickbook::Event, kFixtureEventCount> make_fixture_events() noexcept {
  std::array<tickbook::Event, kFixtureEventCount> events{};
  for (std::size_t i = 0; i < events.size(); ++i) {
    auto& event = events[i];
    event.ts_ns = static_cast<std::int64_t>(i) + 1;
    event.price = i % 2 == 0 ? 10'000 : 10'100;
    event.qty = static_cast<std::int64_t>(i) + 1;
    event.type = i < 2 ? tickbook::EventType::SnapshotLevel : tickbook::EventType::BookUpdate;
    event.side = i % 2 == 0 ? tickbook::Side::Bid : tickbook::Side::Ask;
    event.flags = tickbook::kEventFlagEndOfMessage;
    if (i == 0) event.flags |= tickbook::kEventFlagSnapshotStart;
  }
  return events;
}

bool write_fixture(const std::filesystem::path& path,
                   const std::array<tickbook::Event, kFixtureEventCount>& events) {
  std::array<char, 16> symbol{};
  constexpr std::string_view name = "BTCUSDT";
  std::copy(name.begin(), name.end(), symbol.begin());
  const auto header = tickbook::make_binary_file_header(2, 8, symbol, events.size());

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(reinterpret_cast<const char*>(header.data()),
               static_cast<std::streamsize>(header.size()));
  for (const auto& event : events) {
    const auto bytes = encode_event(event);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  output.close();
  return output.good();
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
  if (argc != 2) {
    std::cerr << "Usage: alloc_check FIXTURE.bin\n";
    return 2;
  }

  const auto events = make_fixture_events();
  const std::filesystem::path fixture_path(argv[1]);
  if (!write_fixture(fixture_path, events)) {
    std::cerr << "cannot write synthetic replay fixture: " << fixture_path << '\n';
    return 1;
  }

  tickbook::BinaryReplay source;
  std::string error;
  if (!source.open(fixture_path, error)) {
    std::cerr << "cannot open replay fixture: " << error << '\n';
    return 1;
  }

  tickbook::SpscRing<tickbook::Event, kRingCapacity> ring;
  tickbook::BookMap book;
  std::atomic<bool> start{false};
  std::atomic<bool> producer_done{false};
  std::atomic<std::size_t> produced{};
  std::thread producer([&]() {
    while (!start.load(std::memory_order_acquire)) { // Pairs with main's release after setup.
      std::this_thread::yield();
    }
    for (;;) {
      CountAllocations measured_source_and_push;
      const auto view = source.next();
      if (!view) break;
      const auto event = view->copy();
      while (!ring.try_push(event)) std::this_thread::yield();
      // Main reads this count after join, so it needs no ordering with the queue.
      produced.fetch_add(1, std::memory_order_relaxed);
    }
    // Release publishes completion after the final push; main pairs with an acquire load.
    producer_done.store(true, std::memory_order_release);
  });

  // Relaxed reset is sequenced before start's release, so the producer cannot count early.
  allocation_count.store(0, std::memory_order_relaxed);
  std::size_t consumed = 0;
  tickbook::Event event{};
  start.store(true, std::memory_order_release); // Publishes the initialized replay state.
  for (;;) {
    bool popped = false;
    {
      CountAllocations measured_pop;
      popped = ring.try_pop(event);
    }
    if (popped) {
      if ((event.flags & tickbook::kEventFlagSnapshotStart) != 0) book.clear();
      book.apply(event); // BookMap is the documented allocation baseline and is exempt here.
      ++consumed;
    } else if (producer_done.load(std::memory_order_acquire)) { // Pairs with producer's release.
      break;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();
  // Join guarantees all counted updates are complete; relaxed load only reads the total.
  const auto transport_allocations = allocation_count.load(std::memory_order_relaxed);

  tickbook::BookMap allocation_positive_control;
  // After join, only this thread updates the counter; relaxed access needs no synchronization.
  allocation_count.store(0, std::memory_order_relaxed);
  {
    CountAllocations measured_baseline;
    allocation_positive_control.apply(events[0]);
  }
  const auto book_map_allocations = allocation_count.load(std::memory_order_relaxed);

  tickbook::FlatBook flat_book(8);
  tickbook::OffsetArrayBook offset_book(256);
  std::size_t flat_book_allocations = 0;
  std::size_t offset_book_allocations = 0;
  allocation_count.store(0, std::memory_order_relaxed);
  {
    CountAllocations measured_flat_book;
    for (const auto& update : events) {
      if ((update.flags & tickbook::kEventFlagSnapshotStart) != 0) flat_book.clear();
      flat_book.apply(update);
    }
  }
  flat_book_allocations = allocation_count.load(std::memory_order_relaxed);

  allocation_count.store(0, std::memory_order_relaxed);
  {
    CountAllocations measured_offset_book;
    for (const auto& update : events) {
      if ((update.flags & tickbook::kEventFlagSnapshotStart) != 0) offset_book.clear();
      offset_book.apply(update);
    }
  }
  offset_book_allocations = allocation_count.load(std::memory_order_relaxed);

  const auto best_bid = book.best_bid();
  const auto best_ask = book.best_ask();
  const bool replay_matches_fixture =
      // Join synchronizes with produced; relaxed is sufficient for this result read.
      produced.load(std::memory_order_relaxed) == events.size() && consumed == events.size() &&
      book.bid_levels() == 1 && book.ask_levels() == 1 && best_bid && best_ask &&
      best_bid->price == 10'000 && best_bid->qty == static_cast<tickbook::Qty>(events.size() - 1) &&
      best_ask->price == 10'100 && best_ask->qty == static_cast<tickbook::Qty>(events.size()) &&
      flat_book.best_bid() == best_bid && flat_book.best_ask() == best_ask &&
      offset_book.best_bid() == best_bid && offset_book.best_ask() == best_ask;

  std::cout << "fixture_events=" << events.size()
            << " transport_allocations=" << transport_allocations
            << " book_map_positive_control_allocations=" << book_map_allocations
            << " flat_book_allocations=" << flat_book_allocations
            << " offset_book_allocations=" << offset_book_allocations << '\n';
  if (!replay_matches_fixture || transport_allocations != 0 || book_map_allocations == 0 ||
      flat_book_allocations != 0 || offset_book_allocations != 0) {
    std::cerr << "allocation check failed\n";
    return 1;
  }
  return 0;
}
