#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "tickbook/features/feature_engine.hpp"

namespace tickbook {

inline constexpr std::size_t kFeatureDumpHeaderSize = 96;
inline constexpr std::size_t kFeatureDumpRecordSize = 112;
inline constexpr std::uint32_t kFeatureDumpVersion = 1;
inline constexpr std::uint32_t kFeatureDumpEndianness = 0x01020304U;
inline constexpr std::uint64_t kFeatureSampleIntervalNs = 100'000'000ULL;
inline constexpr std::array<std::uint64_t, 3> kFeatureForwardHorizonsNs{
    1'000'000'000ULL, 3'000'000'000ULL, 10'000'000'000ULL};
inline constexpr std::array<char, 8> kFeatureDumpMagic{'T', 'B', 'F', 'E', 'A', 'T', '0', '1'};

struct FeatureDumpHeader {
  std::int32_t price_scale{};
  std::int32_t qty_scale{};
  std::array<char, 16> symbol{};
  std::uint64_t sample_count{};
  std::uint64_t sample_interval_ns{kFeatureSampleIntervalNs};
};

struct FeatureDumpRecord {
  std::int64_t ts_ns{};
  FeatureVector features{};
  std::array<std::int64_t, 3> forward_mid_return_q20_ticks{};

  [[nodiscard]] bool operator==(const FeatureDumpRecord&) const noexcept = default;
};

enum class FeatureDumpError : std::uint8_t {
  None,
  HeaderTooSmall,
  WrongMagic,
  UnsupportedVersion,
  WrongEndianness,
  WrongHeaderSize,
  WrongRecordSize,
  WrongFeatureScale,
  WrongHorizons,
  InvalidSampleInterval,
  CountMismatch,
};

namespace feature_dump_detail {

inline void write_u32(std::span<std::byte> bytes, std::size_t offset,
                      std::uint32_t value) noexcept {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    bytes[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xffU);
  }
}

inline void write_u64(std::span<std::byte> bytes, std::size_t offset,
                      std::uint64_t value) noexcept {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    bytes[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xffU);
  }
}

[[nodiscard]] inline std::uint32_t read_u32(std::span<const std::byte> bytes,
                                            std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + i]))
             << (8U * i);
  }
  return value;
}

[[nodiscard]] inline std::uint64_t read_u64(std::span<const std::byte> bytes,
                                            std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + i]))
             << (8U * i);
  }
  return value;
}

inline void write_i64(std::span<std::byte> bytes, std::size_t offset, std::int64_t value) noexcept {
  write_u64(bytes, offset, std::bit_cast<std::uint64_t>(value));
}

[[nodiscard]] inline std::int64_t read_i64(std::span<const std::byte> bytes,
                                           std::size_t offset) noexcept {
  return std::bit_cast<std::int64_t>(read_u64(bytes, offset));
}

inline void write_i32(std::span<std::byte> bytes, std::size_t offset, std::int32_t value) noexcept {
  write_u32(bytes, offset, std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] inline std::int32_t read_i32(std::span<const std::byte> bytes,
                                           std::size_t offset) noexcept {
  return std::bit_cast<std::int32_t>(read_u32(bytes, offset));
}

} // namespace feature_dump_detail

[[nodiscard]] inline std::array<std::byte, kFeatureDumpHeaderSize>
make_feature_dump_header(const FeatureDumpHeader& value) noexcept {
  std::array<std::byte, kFeatureDumpHeaderSize> bytes{};
  for (std::size_t i = 0; i < kFeatureDumpMagic.size(); ++i) {
    bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(kFeatureDumpMagic[i]));
  }
  const auto view = std::span<std::byte>(bytes);
  feature_dump_detail::write_u32(view, 8, kFeatureDumpVersion);
  feature_dump_detail::write_u32(view, 12, kFeatureDumpEndianness);
  feature_dump_detail::write_u32(view, 16, kFeatureDumpHeaderSize);
  feature_dump_detail::write_u32(view, 20, kFeatureDumpRecordSize);
  feature_dump_detail::write_i32(view, 24, value.price_scale);
  feature_dump_detail::write_i32(view, 28, value.qty_scale);
  for (std::size_t i = 0; i < value.symbol.size(); ++i) {
    bytes[32 + i] = static_cast<std::byte>(static_cast<unsigned char>(value.symbol[i]));
  }
  feature_dump_detail::write_u64(view, 48, value.sample_count);
  feature_dump_detail::write_u64(view, 56, value.sample_interval_ns);
  feature_dump_detail::write_u64(view, 64, static_cast<std::uint64_t>(kFeatureScale));
  for (std::size_t i = 0; i < kFeatureForwardHorizonsNs.size(); ++i) {
    feature_dump_detail::write_u64(view, 72 + i * sizeof(std::uint64_t),
                                   kFeatureForwardHorizonsNs[i]);
  }
  return bytes;
}

[[nodiscard]] inline FeatureDumpError
validate_feature_dump_header(std::span<const std::byte> bytes, std::uint64_t file_size,
                             FeatureDumpHeader& output) noexcept {
  output = {};
  if (bytes.size() < kFeatureDumpHeaderSize) return FeatureDumpError::HeaderTooSmall;
  for (std::size_t i = 0; i < kFeatureDumpMagic.size(); ++i) {
    if (bytes[i] != static_cast<std::byte>(static_cast<unsigned char>(kFeatureDumpMagic[i]))) {
      return FeatureDumpError::WrongMagic;
    }
  }
  if (feature_dump_detail::read_u32(bytes, 8) != kFeatureDumpVersion) {
    return FeatureDumpError::UnsupportedVersion;
  }
  if (feature_dump_detail::read_u32(bytes, 12) != kFeatureDumpEndianness) {
    return FeatureDumpError::WrongEndianness;
  }
  if (feature_dump_detail::read_u32(bytes, 16) != kFeatureDumpHeaderSize) {
    return FeatureDumpError::WrongHeaderSize;
  }
  if (feature_dump_detail::read_u32(bytes, 20) != kFeatureDumpRecordSize) {
    return FeatureDumpError::WrongRecordSize;
  }
  if (feature_dump_detail::read_u64(bytes, 64) != static_cast<std::uint64_t>(kFeatureScale)) {
    return FeatureDumpError::WrongFeatureScale;
  }
  for (std::size_t i = 0; i < kFeatureForwardHorizonsNs.size(); ++i) {
    if (feature_dump_detail::read_u64(bytes, 72 + i * sizeof(std::uint64_t)) !=
        kFeatureForwardHorizonsNs[i]) {
      return FeatureDumpError::WrongHorizons;
    }
  }
  const auto sample_count = feature_dump_detail::read_u64(bytes, 48);
  constexpr auto kMaximumCount =
      (std::numeric_limits<std::uint64_t>::max() - kFeatureDumpHeaderSize) / kFeatureDumpRecordSize;
  if (sample_count > kMaximumCount ||
      kFeatureDumpHeaderSize + sample_count * kFeatureDumpRecordSize != file_size) {
    return FeatureDumpError::CountMismatch;
  }
  const auto interval = feature_dump_detail::read_u64(bytes, 56);
  if (interval == 0) return FeatureDumpError::InvalidSampleInterval;

  output.price_scale = feature_dump_detail::read_i32(bytes, 24);
  output.qty_scale = feature_dump_detail::read_i32(bytes, 28);
  for (std::size_t i = 0; i < output.symbol.size(); ++i) {
    output.symbol[i] = static_cast<char>(std::to_integer<unsigned char>(bytes[32 + i]));
  }
  output.sample_count = sample_count;
  output.sample_interval_ns = interval;
  return FeatureDumpError::None;
}

[[nodiscard]] inline std::string_view feature_dump_error_message(FeatureDumpError error) noexcept {
  switch (error) {
  case FeatureDumpError::None: return "no error";
  case FeatureDumpError::HeaderTooSmall: return "feature dump header is truncated";
  case FeatureDumpError::WrongMagic: return "feature dump magic is invalid";
  case FeatureDumpError::UnsupportedVersion: return "feature dump version is unsupported";
  case FeatureDumpError::WrongEndianness: return "feature dump endianness marker is invalid";
  case FeatureDumpError::WrongHeaderSize: return "feature dump header size is invalid";
  case FeatureDumpError::WrongRecordSize: return "feature dump record size is invalid";
  case FeatureDumpError::WrongFeatureScale: return "feature dump fixed-point scale is invalid";
  case FeatureDumpError::WrongHorizons: return "feature dump forward horizons are invalid";
  case FeatureDumpError::InvalidSampleInterval: return "feature dump sample interval is invalid";
  case FeatureDumpError::CountMismatch: return "feature dump sample count does not match file size";
  }
  return "unknown feature dump error";
}

[[nodiscard]] inline std::array<std::byte, kFeatureDumpRecordSize>
encode_feature_dump_record(const FeatureDumpRecord& record) noexcept {
  std::array<std::byte, kFeatureDumpRecordSize> bytes{};
  const auto view = std::span<std::byte>(bytes);
  feature_dump_detail::write_i64(view, 0, record.ts_ns);
  feature_dump_detail::write_i64(view, 8, record.features.spread_ticks);
  feature_dump_detail::write_i32(view, 16, record.features.imbalance_l1_q20);
  feature_dump_detail::write_i32(view, 20, record.features.imbalance_k_q20);
  feature_dump_detail::write_i64(view, 24, record.features.microprice_offset_q20_ticks);
  feature_dump_detail::write_i64(view, 32, record.features.ofi_10);
  feature_dump_detail::write_i64(view, 40, record.features.ofi_50);
  feature_dump_detail::write_i64(view, 48, record.features.ofi_200);
  feature_dump_detail::write_i64(view, 56, record.features.trade_flow_100ms);
  feature_dump_detail::write_i64(view, 64, record.features.trade_flow_1s);
  feature_dump_detail::write_i64(view, 72, record.features.mid_return_100ms_q20_ticks);
  feature_dump_detail::write_i64(view, 80, record.features.mid_return_1s_q20_ticks);
  for (std::size_t i = 0; i < record.forward_mid_return_q20_ticks.size(); ++i) {
    feature_dump_detail::write_i64(view, 88 + i * sizeof(std::int64_t),
                                   record.forward_mid_return_q20_ticks[i]);
  }
  return bytes;
}

[[nodiscard]] inline FeatureDumpRecord
decode_feature_dump_record(std::span<const std::byte, kFeatureDumpRecordSize> bytes) noexcept {
  FeatureDumpRecord record{};
  record.ts_ns = feature_dump_detail::read_i64(bytes, 0);
  record.features.spread_ticks = feature_dump_detail::read_i64(bytes, 8);
  record.features.imbalance_l1_q20 = feature_dump_detail::read_i32(bytes, 16);
  record.features.imbalance_k_q20 = feature_dump_detail::read_i32(bytes, 20);
  record.features.microprice_offset_q20_ticks = feature_dump_detail::read_i64(bytes, 24);
  record.features.ofi_10 = feature_dump_detail::read_i64(bytes, 32);
  record.features.ofi_50 = feature_dump_detail::read_i64(bytes, 40);
  record.features.ofi_200 = feature_dump_detail::read_i64(bytes, 48);
  record.features.trade_flow_100ms = feature_dump_detail::read_i64(bytes, 56);
  record.features.trade_flow_1s = feature_dump_detail::read_i64(bytes, 64);
  record.features.mid_return_100ms_q20_ticks = feature_dump_detail::read_i64(bytes, 72);
  record.features.mid_return_1s_q20_ticks = feature_dump_detail::read_i64(bytes, 80);
  for (std::size_t i = 0; i < record.forward_mid_return_q20_ticks.size(); ++i) {
    record.forward_mid_return_q20_ticks[i] =
        feature_dump_detail::read_i64(bytes, 88 + i * sizeof(std::int64_t));
  }
  return record;
}

} // namespace tickbook
