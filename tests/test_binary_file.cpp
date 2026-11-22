#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "tickbook/binary_file.hpp"

namespace {

std::array<char, 16> test_symbol() {
  std::array<char, 16> symbol{};
  constexpr char kSymbol[] = "BTCUSDT";
  std::copy_n(kSymbol, sizeof(kSymbol) - 1, symbol.begin());
  return symbol;
}

std::vector<std::byte> test_file(std::uint64_t event_count) {
  const auto size = tickbook::kBinaryFileHeaderSize +
                    static_cast<std::size_t>(event_count) * tickbook::kBinaryEventSize;
  std::vector<std::byte> file(size, std::byte{0});
  const auto header = tickbook::make_binary_file_header(2, 8, test_symbol(), event_count);
  std::copy(header.begin(), header.end(), file.begin());
  return file;
}

void overwrite_u32(std::vector<std::byte>& bytes, std::size_t offset,
                   std::uint32_t value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    bytes[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xffU);
  }
}

} // namespace

TEST(BinaryFile, BuildsAndValidatesHeaderAndEventView) {
  auto file = test_file(2);
  tickbook::BinaryFileView view;

  EXPECT_EQ(tickbook::validate_binary_file(file, view), tickbook::BinaryFileError::None);
  EXPECT_EQ(view.price_scale, 2);
  EXPECT_EQ(view.qty_scale, 8);
  EXPECT_EQ(view.symbol, test_symbol());
  EXPECT_EQ(view.event_count, 2);
  EXPECT_EQ(view.event_bytes.size(), 2 * tickbook::kBinaryEventSize);
  EXPECT_EQ(view.event_bytes.data(), file.data() + tickbook::kBinaryFileHeaderSize);
  EXPECT_EQ(file[12], std::byte{0x04});
  EXPECT_EQ(file[13], std::byte{0x03});
  EXPECT_EQ(file[14], std::byte{0x02});
  EXPECT_EQ(file[15], std::byte{0x01});
}

TEST(BinaryFile, RejectsTruncatedHeaderAndWrongMagic) {
  auto short_file = test_file(0);
  short_file.resize(tickbook::kBinaryFileHeaderSize - 1);
  tickbook::BinaryFileView view;
  EXPECT_EQ(tickbook::validate_binary_file(short_file, view),
            tickbook::BinaryFileError::HeaderTooSmall);

  auto bad_magic = test_file(0);
  bad_magic[0] = std::byte{'X'};
  EXPECT_EQ(tickbook::validate_binary_file(bad_magic, view),
            tickbook::BinaryFileError::WrongMagic);
}

TEST(BinaryFile, RejectsUnsupportedVersionEndiannessAndEventSize) {
  tickbook::BinaryFileView view;

  auto bad_version = test_file(0);
  overwrite_u32(bad_version, 8, tickbook::kBinaryFileVersion + 1);
  EXPECT_EQ(tickbook::validate_binary_file(bad_version, view),
            tickbook::BinaryFileError::UnsupportedVersion);

  auto bad_endianness = test_file(0);
  overwrite_u32(bad_endianness, 12, 0x04030201U);
  EXPECT_EQ(tickbook::validate_binary_file(bad_endianness, view),
            tickbook::BinaryFileError::WrongEndianness);

  auto bad_event_size = test_file(0);
  overwrite_u32(bad_event_size, 16, tickbook::kBinaryEventSize - 1);
  EXPECT_EQ(tickbook::validate_binary_file(bad_event_size, view),
            tickbook::BinaryFileError::WrongEventSize);
}

TEST(BinaryFile, RejectsNonzeroReservedBytes) {
  auto file = test_file(0);
  file.back() = std::byte{1};
  tickbook::BinaryFileView view;
  EXPECT_EQ(tickbook::validate_binary_file(file, view),
            tickbook::BinaryFileError::NonzeroReservedBytes);
}

TEST(BinaryFile, RejectsCountMismatchAndOverflow) {
  auto short_payload = test_file(1);
  short_payload.pop_back();
  tickbook::BinaryFileView view;
  EXPECT_EQ(tickbook::validate_binary_file(short_payload, view),
            tickbook::BinaryFileError::EventCountMismatch);

  const auto huge_count = tickbook::make_binary_file_header(
      2, 8, test_symbol(), std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(tickbook::validate_binary_file(huge_count, view),
            tickbook::BinaryFileError::EventCountMismatch);
}

TEST(BinaryFile, ClearsOutputViewOnValidationFailure) {
  auto file = test_file(1);
  tickbook::BinaryFileView view;
  ASSERT_EQ(tickbook::validate_binary_file(file, view), tickbook::BinaryFileError::None);
  ASSERT_EQ(view.event_count, 1);

  file[0] = std::byte{0};
  EXPECT_EQ(tickbook::validate_binary_file(file, view), tickbook::BinaryFileError::WrongMagic);
  EXPECT_EQ(view.event_count, 0);
  EXPECT_TRUE(view.event_bytes.empty());
}

TEST(BinaryFile, ProvidesStableErrorText) {
  EXPECT_EQ(tickbook::binary_file_error_message(tickbook::BinaryFileError::WrongMagic),
            "binary file has the wrong magic");
}
