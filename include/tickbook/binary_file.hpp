#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "tickbook/types.hpp"

namespace tickbook {

inline constexpr std::size_t kBinaryFileHeaderSize = 64;
inline constexpr std::uint32_t kBinaryFileVersion = 1;
inline constexpr std::uint32_t kBinaryEndiannessMarker = 0x01020304U;
inline constexpr std::uint32_t kBinaryEventSize = 48;
inline constexpr std::array<char, 8> kBinaryFileMagic{'T', 'I', 'C', 'K', 'B', 'O', 'O', 'K'};

using BinaryFileHeaderBytes = std::array<std::byte, kBinaryFileHeaderSize>;
static_assert(sizeof(Event) == kBinaryEventSize);
static_assert(sizeof(BinaryFileHeaderBytes) == kBinaryFileHeaderSize);

enum class BinaryFileError : std::uint8_t {
  None,
  HeaderTooSmall,
  WrongMagic,
  UnsupportedVersion,
  WrongEndianness,
  WrongEventSize,
  NonzeroReservedBytes,
  EventCountMismatch,
};

struct BinaryFileView {
  std::int32_t price_scale{};
  std::int32_t qty_scale{};
  std::array<char, 16> symbol{};
  std::uint64_t event_count{};
  // Non-owning payload view; the validated input storage must outlive this object.
  std::span<const std::byte> event_bytes{};
};

namespace binary_file_detail {

inline void write_u32_le(BinaryFileHeaderBytes& out, std::size_t offset,
                         std::uint32_t value) noexcept {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    out[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xffU);
  }
}

inline void write_u64_le(BinaryFileHeaderBytes& out, std::size_t offset,
                         std::uint64_t value) noexcept {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    out[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xffU);
  }
}

[[nodiscard]] inline std::uint32_t read_u32_le(std::span<const std::byte> bytes,
                                               std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + i]))
             << (8U * i);
  }
  return value;
}

[[nodiscard]] inline std::uint64_t read_u64_le(std::span<const std::byte> bytes,
                                               std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + i]))
             << (8U * i);
  }
  return value;
}

} // namespace binary_file_detail

[[nodiscard]] inline BinaryFileHeaderBytes
make_binary_file_header(std::int32_t price_scale, std::int32_t qty_scale,
                        const std::array<char, 16>& symbol, std::uint64_t event_count) noexcept {
  BinaryFileHeaderBytes header{};
  for (std::size_t i = 0; i < kBinaryFileMagic.size(); ++i) {
    header[i] = static_cast<std::byte>(static_cast<unsigned char>(kBinaryFileMagic[i]));
  }

  binary_file_detail::write_u32_le(header, 8, kBinaryFileVersion);
  binary_file_detail::write_u32_le(header, 12, kBinaryEndiannessMarker);
  binary_file_detail::write_u32_le(header, 16, kBinaryEventSize);
  binary_file_detail::write_u32_le(header, 20, std::bit_cast<std::uint32_t>(price_scale));
  binary_file_detail::write_u32_le(header, 24, std::bit_cast<std::uint32_t>(qty_scale));
  for (std::size_t i = 0; i < symbol.size(); ++i) {
    header[28 + i] = static_cast<std::byte>(static_cast<unsigned char>(symbol[i]));
  }
  binary_file_detail::write_u64_le(header, 44, event_count);
  return header;
}

[[nodiscard]] inline BinaryFileError validate_binary_file(std::span<const std::byte> file,
                                                          BinaryFileView& out) noexcept {
  out = {};
  if (file.size() < kBinaryFileHeaderSize) return BinaryFileError::HeaderTooSmall;

  for (std::size_t i = 0; i < kBinaryFileMagic.size(); ++i) {
    const auto expected = static_cast<std::byte>(static_cast<unsigned char>(kBinaryFileMagic[i]));
    if (file[i] != expected) return BinaryFileError::WrongMagic;
  }

  if (binary_file_detail::read_u32_le(file, 8) != kBinaryFileVersion) {
    return BinaryFileError::UnsupportedVersion;
  }
  if (binary_file_detail::read_u32_le(file, 12) != kBinaryEndiannessMarker) {
    return BinaryFileError::WrongEndianness;
  }
  if (binary_file_detail::read_u32_le(file, 16) != kBinaryEventSize) {
    return BinaryFileError::WrongEventSize;
  }

  for (std::size_t i = 52; i < kBinaryFileHeaderSize; ++i) {
    if (file[i] != std::byte{0}) return BinaryFileError::NonzeroReservedBytes;
  }

  const auto event_count = binary_file_detail::read_u64_le(file, 44);
  constexpr auto kMaxCount =
      (std::numeric_limits<std::uint64_t>::max() - kBinaryFileHeaderSize) / kBinaryEventSize;
  if (event_count > kMaxCount) return BinaryFileError::EventCountMismatch;
  const auto expected_size = kBinaryFileHeaderSize + event_count * kBinaryEventSize;
  if (expected_size != file.size()) return BinaryFileError::EventCountMismatch;

  out.price_scale = std::bit_cast<std::int32_t>(binary_file_detail::read_u32_le(file, 20));
  out.qty_scale = std::bit_cast<std::int32_t>(binary_file_detail::read_u32_le(file, 24));
  for (std::size_t i = 0; i < out.symbol.size(); ++i) {
    out.symbol[i] = static_cast<char>(std::to_integer<unsigned char>(file[28 + i]));
  }
  out.event_count = event_count;
  out.event_bytes = file.subspan(kBinaryFileHeaderSize);
  return BinaryFileError::None;
}

[[nodiscard]] constexpr std::string_view binary_file_error_message(BinaryFileError error) noexcept {
  switch (error) {
  case BinaryFileError::None: return "no error";
  case BinaryFileError::HeaderTooSmall: return "binary file is shorter than its header";
  case BinaryFileError::WrongMagic: return "binary file has the wrong magic";
  case BinaryFileError::UnsupportedVersion: return "binary file version is unsupported";
  case BinaryFileError::WrongEndianness: return "binary file endianness marker is invalid";
  case BinaryFileError::WrongEventSize: return "binary file event size is unsupported";
  case BinaryFileError::NonzeroReservedBytes:
    return "binary file reserved header bytes are nonzero";
  case BinaryFileError::EventCountMismatch: return "binary event count does not match file size";
  }
  return "unknown binary file error";
}

} // namespace tickbook
