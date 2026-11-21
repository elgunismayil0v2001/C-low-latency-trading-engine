#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

#include "tickbook/fixed.hpp"

using tickbook::parse_fixed;

static bool parse(std::string_view s, int scale, std::int64_t& out) { return parse_fixed(s, scale, out); }

TEST(ParseFixed, ParsesPlainDecimal) {
  std::int64_t v = 0;
  EXPECT_TRUE(parse("43000.12", 2, v));
  EXPECT_EQ(v, 4300012);
}

TEST(ParseFixed, PadsMissingFraction) {
  std::int64_t v = 0;
  EXPECT_TRUE(parse("100", 8, v));
  EXPECT_EQ(v, 10000000000);
  EXPECT_TRUE(parse("0.5", 3, v));
  EXPECT_EQ(v, 500);
}

TEST(ParseFixed, AcceptsTrailingZerosBeyondScale) {
  std::int64_t v = 0;
  EXPECT_TRUE(parse("43000.12000000", 2, v));
  EXPECT_EQ(v, 4300012);
}

TEST(ParseFixed, RejectsLostPrecision) {
  std::int64_t v = 0;
  EXPECT_FALSE(parse("1.123", 2, v));
}

TEST(ParseFixed, RejectsBadInput) {
  std::int64_t v = 0;
  EXPECT_FALSE(parse("", 2, v));
  EXPECT_FALSE(parse(".", 2, v));
  EXPECT_FALSE(parse("1.2.3", 2, v));
  EXPECT_FALSE(parse("abc", 2, v));
  EXPECT_FALSE(parse("-1.0", 2, v));
}

TEST(ParseFixed, RejectsOverflow) {
  std::int64_t v = 0;
  EXPECT_FALSE(parse("99999999999999999999", 2, v));
  EXPECT_FALSE(parse("9223372036854775807", 2, v)); // fits as an integer, overflows after scaling
}
