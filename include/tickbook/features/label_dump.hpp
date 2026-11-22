#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "tickbook/features/feature_dump.hpp"

namespace tickbook {

inline constexpr std::size_t kLabelDumpHeaderSize = 64;
inline constexpr std::size_t kLabelDumpRecordSize = 96;
inline constexpr std::uint32_t kLabelDumpVersion = 1;
inline constexpr std::array<char, 8> kLabelDumpMagic{'T', 'B', 'L', 'A', 'B', 'E', 'L', '1'};

enum class LabelKind : std::uint8_t {
  Direction = 1,
  Return = 2,
};

struct LabelDumpHeader {
  LabelKind kind{LabelKind::Direction};
  std::uint32_t horizon_seconds{};
  std::int64_t dead_zone_ticks{};
  std::uint64_t sample_count{};
  std::array<char, 16> symbol{};
};

struct LabelDumpRecord {
  std::int64_t ts_ns{};
  FeatureVector features{};
  // Direction values are down=0, flat=1, up=2; return values are Q20 ticks.
  std::int64_t label{};

  [[nodiscard]] bool operator==(const LabelDumpRecord&) const noexcept = default;
};

enum class LabelDumpError : std::uint8_t {
  None,
  HeaderTooSmall,
  WrongMagic,
  UnsupportedVersion,
  WrongEndianness,
  WrongHeaderSize,
  WrongRecordSize,
  InvalidKind,
  InvalidHorizon,
  InvalidDeadZone,
  CountMismatch,
};

[[nodiscard]] inline std::array<std::byte, kLabelDumpHeaderSize>
make_label_dump_header(const LabelDumpHeader& value) noexcept {
  std::array<std::byte, kLabelDumpHeaderSize> bytes{};
  for (std::size_t index = 0; index < kLabelDumpMagic.size(); ++index) {
    bytes[index] = static_cast<std::byte>(static_cast<unsigned char>(kLabelDumpMagic[index]));
  }
  auto view = std::span<std::byte>(bytes);
  feature_dump_detail::write_u32(view, 8, kLabelDumpVersion);
  feature_dump_detail::write_u32(view, 12, kFeatureDumpEndianness);
  feature_dump_detail::write_u32(view, 16, kLabelDumpHeaderSize);
  feature_dump_detail::write_u32(view, 20, kLabelDumpRecordSize);
  feature_dump_detail::write_u32(view, 24, static_cast<std::uint32_t>(value.kind));
  feature_dump_detail::write_u32(view, 28, value.horizon_seconds);
  feature_dump_detail::write_i64(view, 32, value.dead_zone_ticks);
  feature_dump_detail::write_u64(view, 40, value.sample_count);
  for (std::size_t index = 0; index < value.symbol.size(); ++index) {
    bytes[48 + index] = static_cast<std::byte>(static_cast<unsigned char>(value.symbol[index]));
  }
  return bytes;
}

[[nodiscard]] inline LabelDumpError validate_label_dump_header(std::span<const std::byte> bytes,
                                                               std::uint64_t file_size,
                                                               LabelDumpHeader& output) noexcept {
  output = {};
  if (bytes.size() < kLabelDumpHeaderSize) return LabelDumpError::HeaderTooSmall;
  for (std::size_t index = 0; index < kLabelDumpMagic.size(); ++index) {
    if (bytes[index] !=
        static_cast<std::byte>(static_cast<unsigned char>(kLabelDumpMagic[index]))) {
      return LabelDumpError::WrongMagic;
    }
  }
  if (feature_dump_detail::read_u32(bytes, 8) != kLabelDumpVersion) {
    return LabelDumpError::UnsupportedVersion;
  }
  if (feature_dump_detail::read_u32(bytes, 12) != kFeatureDumpEndianness) {
    return LabelDumpError::WrongEndianness;
  }
  if (feature_dump_detail::read_u32(bytes, 16) != kLabelDumpHeaderSize) {
    return LabelDumpError::WrongHeaderSize;
  }
  if (feature_dump_detail::read_u32(bytes, 20) != kLabelDumpRecordSize) {
    return LabelDumpError::WrongRecordSize;
  }
  const auto raw_kind = feature_dump_detail::read_u32(bytes, 24);
  if (raw_kind != static_cast<std::uint32_t>(LabelKind::Direction) &&
      raw_kind != static_cast<std::uint32_t>(LabelKind::Return)) {
    return LabelDumpError::InvalidKind;
  }
  const auto horizon = feature_dump_detail::read_u32(bytes, 28);
  if (horizon != 1 && horizon != 3 && horizon != 10) return LabelDumpError::InvalidHorizon;
  const auto dead_zone = feature_dump_detail::read_i64(bytes, 32);
  if ((raw_kind == static_cast<std::uint32_t>(LabelKind::Direction) && dead_zone < 1) ||
      (raw_kind == static_cast<std::uint32_t>(LabelKind::Return) && dead_zone != 0)) {
    return LabelDumpError::InvalidDeadZone;
  }
  const auto count = feature_dump_detail::read_u64(bytes, 40);
  constexpr auto kMaximumCount =
      (std::numeric_limits<std::uint64_t>::max() - kLabelDumpHeaderSize) / kLabelDumpRecordSize;
  if (count > kMaximumCount || kLabelDumpHeaderSize + count * kLabelDumpRecordSize != file_size) {
    return LabelDumpError::CountMismatch;
  }

  output.kind = static_cast<LabelKind>(raw_kind);
  output.horizon_seconds = horizon;
  output.dead_zone_ticks = dead_zone;
  output.sample_count = count;
  for (std::size_t index = 0; index < output.symbol.size(); ++index) {
    output.symbol[index] = static_cast<char>(std::to_integer<unsigned char>(bytes[48 + index]));
  }
  return LabelDumpError::None;
}

[[nodiscard]] inline std::array<std::byte, kLabelDumpRecordSize>
encode_label_dump_record(const LabelDumpRecord& value) noexcept {
  std::array<std::byte, kLabelDumpRecordSize> bytes{};
  const FeatureDumpRecord feature_record{value.ts_ns, value.features, {}};
  const auto encoded_features = encode_feature_dump_record(feature_record);
  std::copy_n(encoded_features.begin(), 88, bytes.begin());
  feature_dump_detail::write_i64(std::span<std::byte>(bytes), 88, value.label);
  return bytes;
}

[[nodiscard]] inline LabelDumpRecord
decode_label_dump_record(std::span<const std::byte, kLabelDumpRecordSize> bytes) noexcept {
  std::array<std::byte, kFeatureDumpRecordSize> feature_bytes{};
  std::copy_n(bytes.begin(), 88, feature_bytes.begin());
  const auto feature_record = decode_feature_dump_record(feature_bytes);
  return {feature_record.ts_ns, feature_record.features, feature_dump_detail::read_i64(bytes, 88)};
}

[[nodiscard]] inline std::string_view label_dump_error_message(LabelDumpError error) noexcept {
  switch (error) {
  case LabelDumpError::None: return "no error";
  case LabelDumpError::HeaderTooSmall: return "label dump header is truncated";
  case LabelDumpError::WrongMagic: return "label dump magic is invalid";
  case LabelDumpError::UnsupportedVersion: return "label dump version is unsupported";
  case LabelDumpError::WrongEndianness: return "label dump endianness marker is invalid";
  case LabelDumpError::WrongHeaderSize: return "label dump header size is invalid";
  case LabelDumpError::WrongRecordSize: return "label dump record size is invalid";
  case LabelDumpError::InvalidKind: return "label dump kind is invalid";
  case LabelDumpError::InvalidHorizon: return "label dump horizon is invalid";
  case LabelDumpError::InvalidDeadZone: return "label dump dead zone is invalid";
  case LabelDumpError::CountMismatch: return "label dump count does not match file size";
  }
  return "unknown label dump error";
}

} // namespace tickbook
