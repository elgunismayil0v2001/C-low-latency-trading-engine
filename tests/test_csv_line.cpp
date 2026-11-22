#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

#include "tickbook/csv_line.hpp"

TEST(CsvLine, SplitsEightFieldsInPlace) {
  std::string line = "binance,BTCUSDT,123,124,true,bid,100.00,0.25";
  std::array<std::string_view, tickbook::csv_detail::kCsvFieldCount> fields{};

  ASSERT_TRUE(tickbook::csv_detail::split_csv_line(line, fields));
  EXPECT_EQ(fields[0], "binance");
  EXPECT_EQ(fields[1], "BTCUSDT");
  EXPECT_EQ(fields[2], "123");
  EXPECT_EQ(fields[7], "0.25");
}

TEST(CsvLine, DecodesQuotedCommasAndEscapedQuotes) {
  std::string line = "\"bin,ance\",\"BTC\"\"USDT\",1,2,false,bid,99,1";
  std::array<std::string_view, tickbook::csv_detail::kCsvFieldCount> fields{};

  ASSERT_TRUE(tickbook::csv_detail::split_csv_line(line, fields));
  EXPECT_EQ(fields[0], "bin,ance");
  EXPECT_EQ(fields[1], "BTC\"USDT");
}

TEST(CsvLine, AcceptsAnEmptyLastField) {
  std::string line = "a,b,c,d,e,f,g,";
  std::array<std::string_view, tickbook::csv_detail::kCsvFieldCount> fields{};

  ASSERT_TRUE(tickbook::csv_detail::split_csv_line(line, fields));
  EXPECT_TRUE(fields[7].empty());
}

TEST(CsvLine, RejectsMalformedQuotingAndFieldCounts) {
  const std::array<std::string_view, 5> invalid{"a,b,c,d,e,f,g", "a,b,c,d,e,f,g,h,i",
                                                "a,b,\"c,d,e,f,g,h", "a,b,\"c\"x,d,e,f,g,h",
                                                "a,b,c\"d,e,f,g,h,i"};

  for (const auto input : invalid) {
    std::string line(input);
    std::array<std::string_view, tickbook::csv_detail::kCsvFieldCount> fields{};
    EXPECT_FALSE(tickbook::csv_detail::split_csv_line(line, fields)) << input;
  }
}
