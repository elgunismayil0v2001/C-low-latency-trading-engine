#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "tickbook/features/label_dump.hpp"

namespace tickbook::label_tool {

class ILabeler {
public:
  virtual ~ILabeler() = default;
  [[nodiscard]] virtual LabelKind kind() const noexcept = 0;
  [[nodiscard]] virtual std::int64_t dead_zone_ticks() const noexcept = 0;
  [[nodiscard]] virtual std::int64_t label(const FeatureDumpRecord& record,
                                           std::size_t horizon_index) const noexcept = 0;
};

class DirectionLabeler final : public ILabeler {
public:
  explicit DirectionLabeler(std::int64_t dead_zone_ticks = 1) noexcept;

  [[nodiscard]] LabelKind kind() const noexcept override { return LabelKind::Direction; }
  [[nodiscard]] std::int64_t dead_zone_ticks() const noexcept override { return dead_zone_ticks_; }
  [[nodiscard]] std::int64_t label(const FeatureDumpRecord& record,
                                   std::size_t horizon_index) const noexcept override;
  [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
  std::int64_t dead_zone_ticks_{};
  std::int64_t dead_zone_q20_{};
  bool valid_{};
};

class ReturnLabeler final : public ILabeler {
public:
  [[nodiscard]] LabelKind kind() const noexcept override { return LabelKind::Return; }
  [[nodiscard]] std::int64_t dead_zone_ticks() const noexcept override { return 0; }
  [[nodiscard]] std::int64_t label(const FeatureDumpRecord& record,
                                   std::size_t horizon_index) const noexcept override;
};

struct Options {
  std::filesystem::path input_path;
  std::filesystem::path output_path;
  const ILabeler* labeler{};
  std::uint32_t horizon_seconds{3};
};

struct Result {
  bool success{};
  std::uint64_t sample_count{};
  std::string error;
};

[[nodiscard]] Result label_feature_file(const Options& options);

} // namespace tickbook::label_tool
