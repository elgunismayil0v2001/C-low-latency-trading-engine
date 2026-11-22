#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>

#include "tickbook/latency_histogram.hpp"

TEST(LatencyHistogram, ReportsPercentilesFromPreallocatedLogLinearBuckets) {
  tickbook::LatencyHistogram histogram;
  EXPECT_EQ(tickbook::LatencyHistogram::kBucketCount, 1920U);
  EXPECT_EQ(histogram.count(), 0U);
  EXPECT_FALSE(histogram.percentile_upper_ns(5000));

  constexpr std::array<std::uint64_t, 12> samples{0, 1, 2, 31, 32, 33, 63, 64, 127, 128, 255, 256};
  for (const auto sample : samples) histogram.record(sample);

  EXPECT_EQ(histogram.count(), samples.size());
  EXPECT_EQ(histogram.maximum_ns(), 256U);
  EXPECT_EQ(histogram.percentile_upper_ns(5000), 33U);
  EXPECT_EQ(histogram.percentile_upper_ns(9900), 263U);
  EXPECT_EQ(histogram.percentile_upper_ns(9990), 263U);
  EXPECT_EQ(histogram.percentile_upper_ns(10'000), 263U);
  EXPECT_FALSE(histogram.percentile_upper_ns(0));
  EXPECT_FALSE(histogram.percentile_upper_ns(10'001));
}

TEST(LatencyHistogram, ResetClearsCountsAndMaximumWithoutChangingCapacity) {
  tickbook::LatencyHistogram histogram;
  histogram.record(42);
  histogram.reset();

  EXPECT_EQ(histogram.count(), 0U);
  EXPECT_EQ(histogram.maximum_ns(), 0U);
  EXPECT_FALSE(histogram.percentile_upper_ns(5000));
}

TEST(LatencyHistogram, HandlesTheLargestNanosecondValue) {
  tickbook::LatencyHistogram histogram;
  histogram.record(std::numeric_limits<std::uint64_t>::max());

  EXPECT_EQ(histogram.count(), 1U);
  EXPECT_EQ(histogram.maximum_ns(), std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(histogram.percentile_upper_ns(10'000), std::numeric_limits<std::uint64_t>::max());
}

TEST(LatencyHistogram, SubtractsTimerOverheadAndSaturatesAtZero) {
  EXPECT_EQ(tickbook::latency_detail::subtract_timer_overhead(20, 8), 12U);
  EXPECT_EQ(tickbook::latency_detail::subtract_timer_overhead(8, 8), 0U);
  EXPECT_EQ(tickbook::latency_detail::subtract_timer_overhead(3, 8), 0U);
}
