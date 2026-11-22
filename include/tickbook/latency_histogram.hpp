#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace tickbook {

namespace latency_detail {

[[nodiscard]] constexpr std::uint64_t subtract_timer_overhead(std::uint64_t elapsed_ns,
                                                              std::uint64_t overhead_ns) noexcept {
  return elapsed_ns > overhead_ns ? elapsed_ns - overhead_ns : 0;
}

} // namespace latency_detail

// Exact nanosecond buckets cover [0, 31]. Higher values use 32 sub-buckets per
// power of two, keeping storage fixed while preserving useful tail resolution.
class LatencyHistogram {
public:
  static constexpr std::size_t kLinearBucketCount = 32;
  static constexpr std::size_t kSubBucketsPerExponent = 32;
  static constexpr std::size_t kBucketCount =
      kLinearBucketCount + (64 - 5) * kSubBucketsPerExponent;

  void reset() noexcept {
    buckets_.fill(0);
    count_ = 0;
    maximum_ns_ = 0;
  }

  void record(std::uint64_t elapsed_ns) noexcept {
    ++buckets_[bucket_index(elapsed_ns)];
    ++count_;
    maximum_ns_ = std::max(maximum_ns_, elapsed_ns);
  }

  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
  [[nodiscard]] std::uint64_t maximum_ns() const noexcept { return maximum_ns_; }

  // Percentiles are returned as the upper edge of the matching log-linear bucket.
  // Use basis points so 9990 represents p99.9 without floating-point arithmetic.
  [[nodiscard]] std::optional<std::uint64_t>
  percentile_upper_ns(std::uint32_t percentile_basis_points) const noexcept {
    if (count_ == 0 || percentile_basis_points == 0 || percentile_basis_points > 10'000) {
      return std::nullopt;
    }

    const std::uint64_t whole = count_ / 10'000;
    const std::uint64_t remainder = count_ % 10'000;
    const std::uint64_t rank =
        whole * percentile_basis_points + (remainder * percentile_basis_points + 9'999) / 10'000;
    std::uint64_t cumulative = 0;
    for (std::size_t index = 0; index < buckets_.size(); ++index) {
      cumulative += buckets_[index];
      if (cumulative >= rank) return bucket_upper_ns(index);
    }
    return maximum_ns_;
  }

private:
  [[nodiscard]] static std::size_t bucket_index(std::uint64_t elapsed_ns) noexcept {
    if (elapsed_ns < kLinearBucketCount) return static_cast<std::size_t>(elapsed_ns);
    const unsigned int exponent = std::bit_width(elapsed_ns) - 1U;
    const std::uint64_t base = std::uint64_t{1} << exponent;
    const std::uint64_t step = std::uint64_t{1} << (exponent - 5U);
    const auto sub_bucket = static_cast<std::size_t>((elapsed_ns - base) / step);
    return kLinearBucketCount + (exponent - 5U) * kSubBucketsPerExponent + sub_bucket;
  }

  [[nodiscard]] static std::uint64_t bucket_upper_ns(std::size_t index) noexcept {
    if (index < kLinearBucketCount) return static_cast<std::uint64_t>(index);
    const std::size_t offset = index - kLinearBucketCount;
    const unsigned int exponent = 5U + static_cast<unsigned int>(offset / kSubBucketsPerExponent);
    const std::size_t sub_bucket = offset % kSubBucketsPerExponent;
    const std::uint64_t base = std::uint64_t{1} << exponent;
    const std::uint64_t step = std::uint64_t{1} << (exponent - 5U);
    const std::uint64_t lower = base + sub_bucket * step;
    return lower | (step - 1U);
  }

  std::array<std::uint64_t, kBucketCount> buckets_{};
  std::uint64_t count_{};
  std::uint64_t maximum_ns_{};
};

} // namespace tickbook
