#pragma once

#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tickbook/binary_file.hpp"
#include "tickbook/types.hpp"

namespace tickbook {

// A non-owning view over one serialized Event. The mapped bytes remain valid until
// the owning BinaryReplay is closed, reopened, or destroyed.
class BinaryEventView {
public:
  // Fixed extent bounds accessor reads; callers validate the enclosing file first.
  explicit BinaryEventView(std::span<const std::byte, kBinaryEventSize> bytes) noexcept
      : bytes_(bytes.data()) {}

  [[nodiscard]] std::span<const std::byte, kBinaryEventSize> bytes() const noexcept {
    return std::span<const std::byte, kBinaryEventSize>(bytes_, kBinaryEventSize);
  }
  [[nodiscard]] std::int64_t ts_ns() const noexcept {
    return std::bit_cast<std::int64_t>(read_u64(0));
  }
  [[nodiscard]] std::uint64_t first_id() const noexcept { return read_u64(8); }
  [[nodiscard]] std::uint64_t last_id() const noexcept { return read_u64(16); }
  [[nodiscard]] Price price() const noexcept { return std::bit_cast<Price>(read_u64(24)); }
  [[nodiscard]] Qty qty() const noexcept { return std::bit_cast<Qty>(read_u64(32)); }
  [[nodiscard]] EventType type() const noexcept {
    return static_cast<EventType>(std::to_integer<std::uint8_t>(bytes_[40]));
  }
  [[nodiscard]] Side side() const noexcept {
    return static_cast<Side>(std::to_integer<std::uint8_t>(bytes_[41]));
  }
  [[nodiscard]] std::uint8_t flags() const noexcept {
    return std::to_integer<std::uint8_t>(bytes_[42]);
  }
  [[nodiscard]] std::uint8_t padding(std::size_t index) const noexcept {
    return index < 5 ? std::to_integer<std::uint8_t>(bytes_[43 + index]) : 0;
  }

  // Materialize only when a caller needs an owned Event, for example to push it
  // into the replay ring. Iterating the mapped file does not call this.
  [[nodiscard]] Event copy() const noexcept {
    Event event{};
    event.ts_ns = ts_ns();
    event.first_id = first_id();
    event.last_id = last_id();
    event.price = price();
    event.qty = qty();
    event.type = type();
    event.side = side();
    event.flags = flags();
    for (std::size_t i = 0; i < std::size(event.padding); ++i) { event.padding[i] = padding(i); }
    return event;
  }

private:
  [[nodiscard]] std::uint64_t read_u64(std::size_t offset) const noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(value); ++i) {
      value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes_[offset + i]))
               << (8U * i);
    }
    return value;
  }

  const std::byte* bytes_{};
  friend class BinaryReplay;
};

// Memory-mapped, read-only access to a validated binary event file.
class BinaryReplay {
public:
  BinaryReplay() = default;
  BinaryReplay(const BinaryReplay&) = delete;
  BinaryReplay& operator=(const BinaryReplay&) = delete;

  BinaryReplay(BinaryReplay&& other) noexcept { take(other); }
  BinaryReplay& operator=(BinaryReplay&& other) noexcept {
    if (this != &other) {
      close();
      take(other);
    }
    return *this;
  }

  ~BinaryReplay() { close(); }

  [[nodiscard]] bool open(const std::filesystem::path& path, std::string& error,
                          bool lock_pages = false) {
    close();
    error.clear();
    if (path.empty()) {
      error = "binary replay path is empty";
      return false;
    }

    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
      error = system_error("cannot open binary replay", path, errno);
      return false;
    }

    struct stat info {};
    if (::fstat(descriptor, &info) != 0) {
      const int error_code = errno;
      ::close(descriptor);
      error = system_error("cannot stat binary replay", path, error_code);
      return false;
    }
    if (info.st_size < 0 ||
        static_cast<std::uintmax_t>(info.st_size) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
      ::close(descriptor);
      error = "binary replay file is too large to map";
      return false;
    }
    const auto size = static_cast<std::size_t>(info.st_size);
    if (size < kBinaryFileHeaderSize) {
      ::close(descriptor);
      error = std::string(binary_file_error_message(BinaryFileError::HeaderTooSmall));
      return false;
    }

    int map_flags = MAP_PRIVATE;
#if defined(__linux__) && defined(MAP_POPULATE)
    map_flags |= MAP_POPULATE;
#endif
    void* mapping = ::mmap(nullptr, size, PROT_READ, map_flags, descriptor, 0);
    const int map_error = errno;
    ::close(descriptor);
    if (mapping == MAP_FAILED) {
      error = system_error("cannot map binary replay", path, map_error);
      return false;
    }

    const auto file = std::span<const std::byte>(static_cast<const std::byte*>(mapping), size);
    BinaryFileView validated{};
    const auto validation = validate_binary_file(file, validated);
    if (validation != BinaryFileError::None) {
      ::munmap(mapping, size);
      error = std::string(binary_file_error_message(validation));
      return false;
    }
    if (lock_pages) {
      if (::mlock(mapping, size) != 0) {
        const int lock_error = errno;
        ::munmap(mapping, size);
        error = system_error("cannot lock binary replay pages", path, lock_error);
        return false;
      }
      pages_locked_ = true;
    }

    mapping_ = mapping;
    mapped_size_ = size;
    file_view_ = validated;
    next_index_ = 0;
    return true;
  }

  void close() noexcept {
    if (mapping_ != nullptr) {
      if (pages_locked_) ::munlock(mapping_, mapped_size_);
      ::munmap(mapping_, mapped_size_);
    }
    mapping_ = nullptr;
    mapped_size_ = 0;
    pages_locked_ = false;
    file_view_ = {};
    next_index_ = 0;
  }

  [[nodiscard]] std::optional<BinaryEventView> next() noexcept {
    if (mapping_ == nullptr || next_index_ >= file_view_.event_count) return std::nullopt;
    const auto* begin = file_view_.event_bytes.data() + next_index_ * kBinaryEventSize;
    ++next_index_;
    return BinaryEventView(std::span<const std::byte, kBinaryEventSize>(begin, kBinaryEventSize));
  }

  void rewind() noexcept { next_index_ = 0; }

  [[nodiscard]] bool is_open() const noexcept { return mapping_ != nullptr; }
  [[nodiscard]] bool pages_locked() const noexcept { return pages_locked_; }
  [[nodiscard]] std::uint64_t event_count() const noexcept { return file_view_.event_count; }
  [[nodiscard]] std::int32_t price_scale() const noexcept { return file_view_.price_scale; }
  [[nodiscard]] std::int32_t qty_scale() const noexcept { return file_view_.qty_scale; }
  [[nodiscard]] const std::array<char, 16>& symbol() const noexcept { return file_view_.symbol; }
  [[nodiscard]] std::size_t mapped_size() const noexcept { return mapped_size_; }

private:
  [[nodiscard]] static std::string system_error(std::string_view operation,
                                                const std::filesystem::path& path, int error_code) {
    return std::string(operation) + " '" + path.string() +
           "': " + std::error_code(error_code, std::generic_category()).message();
  }

  void take(BinaryReplay& other) noexcept {
    mapping_ = std::exchange(other.mapping_, nullptr);
    mapped_size_ = std::exchange(other.mapped_size_, 0);
    pages_locked_ = std::exchange(other.pages_locked_, false);
    file_view_ = other.file_view_;
    next_index_ = std::exchange(other.next_index_, 0);
    other.file_view_ = {};
  }

  void* mapping_{};
  std::size_t mapped_size_{};
  bool pages_locked_{};
  BinaryFileView file_view_{};
  std::uint64_t next_index_{};
};

} // namespace tickbook
