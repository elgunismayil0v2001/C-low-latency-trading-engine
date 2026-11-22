#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

#include "tickbook/nn/mlp.hpp"

namespace {

std::vector<std::byte> fixture_bytes() {
  const auto path = std::filesystem::path(TICKBOOK_TEST_DATA_DIR) / "tiny_weights.bin";
  std::ifstream input(path, std::ios::binary);
  std::vector<char> chars((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  std::vector<std::byte> result(chars.size());
  for (std::size_t index = 0; index < chars.size(); ++index) {
    result[index] = static_cast<std::byte>(static_cast<unsigned char>(chars[index]));
  }
  return result;
}

void write_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

void refresh_crc(std::vector<std::byte>& bytes) {
  const auto crc = tickbook::nn::weights_detail::crc32(
      std::span<const std::byte>(bytes).first(bytes.size() - sizeof(std::uint32_t)));
  write_u32(bytes, bytes.size() - sizeof(std::uint32_t), crc);
}

tickbook::nn::WeightsLoadError load(std::vector<std::byte> bytes) {
  tickbook::nn::MlpModel model{};
  return tickbook::nn::load_weights(bytes, model);
}

} // namespace

TEST(WeightsLoader, LoadsValidatedTinyMlpAndInfersWithoutDynamicStorage) {
  const auto bytes = fixture_bytes();
  ASSERT_FALSE(bytes.empty());
  tickbook::nn::MlpModel model{};
  ASSERT_EQ(tickbook::nn::load_weights(bytes, model), tickbook::nn::WeightsLoadError::None);
  EXPECT_EQ(model.output_width(), 3U);
  EXPECT_EQ(model.layers[0].input_width, tickbook::nn::kMlpFeatureCount);
  EXPECT_EQ(model.layers[0].output_width, 16U);
  EXPECT_EQ(model.layers[1].output_width, 16U);

  tickbook::FeatureVector input{};
  input.spread_ticks = 2;
  input.imbalance_l1_q20 = -120'000;
  input.imbalance_k_q20 = 230'000;
  input.microprice_offset_q20_ticks = 45'000;
  input.ofi_10 = 12'345;
  input.ofi_50 = -23'456;
  input.ofi_200 = 34'567;
  input.trade_flow_100ms = -45'678;
  input.trade_flow_1s = 56'789;
  input.mid_return_100ms_q20_ticks = -67'890;
  input.mid_return_1s_q20_ticks = 78'901;
  const auto output = model.infer(input);
  EXPECT_EQ(output.count, 3U);
  for (const auto value : output.values) EXPECT_TRUE(std::isfinite(value));

  for (std::int64_t sample = 0; sample < 1000; ++sample) {
    auto varied = input;
    const auto value = (sample * 104'729) % 2'000'001 - 1'000'000;
    varied.spread_ticks = sample % 7;
    varied.imbalance_l1_q20 = static_cast<std::int32_t>(value);
    varied.imbalance_k_q20 = static_cast<std::int32_t>(-value / 2);
    varied.microprice_offset_q20_ticks = value * 3;
    varied.ofi_10 = value * 5;
    varied.ofi_50 = -value * 7;
    varied.ofi_200 = value * 11;
    varied.trade_flow_100ms = -value * 13;
    varied.trade_flow_1s = value * 17;
    varied.mid_return_100ms_q20_ticks = -value * 19;
    varied.mid_return_1s_q20_ticks = value * 23;
    const auto vectorized = model.infer(varied);
    const auto scalar = model.infer_scalar(varied);
    ASSERT_EQ(vectorized.count, scalar.count);
    for (std::size_t index = 0; index < scalar.count; ++index) {
      EXPECT_NEAR(vectorized.values[index], scalar.values[index], 1e-5F);
    }
  }
}

TEST(WeightsLoader, RejectsHeaderDimensionActivationAndChecksumErrors) {
  const auto valid = fixture_bytes();
  ASSERT_FALSE(valid.empty());

  auto wrong_magic = valid;
  wrong_magic[0] = std::byte{0};
  EXPECT_EQ(load(std::move(wrong_magic)), tickbook::nn::WeightsLoadError::WrongMagic);

  auto wrong_version = valid;
  write_u32(wrong_version, 8, 99);
  EXPECT_EQ(load(std::move(wrong_version)), tickbook::nn::WeightsLoadError::UnsupportedVersion);

  auto wrong_endian = valid;
  write_u32(wrong_endian, 12, 0);
  EXPECT_EQ(load(std::move(wrong_endian)), tickbook::nn::WeightsLoadError::WrongEndianness);

  auto wrong_layer_count = valid;
  write_u32(wrong_layer_count, 16, 2);
  EXPECT_EQ(load(std::move(wrong_layer_count)), tickbook::nn::WeightsLoadError::WrongLayerCount);

  auto wrong_dimensions = valid;
  write_u32(wrong_dimensions, 20, 10);
  refresh_crc(wrong_dimensions);
  EXPECT_EQ(load(std::move(wrong_dimensions)), tickbook::nn::WeightsLoadError::InvalidDimensions);

  auto wrong_activation = valid;
  write_u32(wrong_activation, 28, 99);
  refresh_crc(wrong_activation);
  EXPECT_EQ(load(std::move(wrong_activation)), tickbook::nn::WeightsLoadError::InvalidActivation);

  auto wrong_size = valid;
  wrong_size.push_back(std::byte{0});
  EXPECT_EQ(load(std::move(wrong_size)), tickbook::nn::WeightsLoadError::SizeMismatch);

  auto wrong_crc = valid;
  wrong_crc.back() ^= std::byte{1};
  EXPECT_EQ(load(std::move(wrong_crc)), tickbook::nn::WeightsLoadError::ChecksumMismatch);
}

TEST(WeightsLoader, RejectsInvalidNormalizationAndNonFiniteParameters) {
  const auto valid = fixture_bytes();
  ASSERT_FALSE(valid.empty());

  auto non_finite_mean = valid;
  write_u32(non_finite_mean, 56, 0x7fc00000U);
  refresh_crc(non_finite_mean);
  EXPECT_EQ(load(std::move(non_finite_mean)),
            tickbook::nn::WeightsLoadError::NonFiniteNormalization);

  auto zero_standard_deviation = valid;
  write_u32(zero_standard_deviation, 100, 0);
  refresh_crc(zero_standard_deviation);
  EXPECT_EQ(load(std::move(zero_standard_deviation)),
            tickbook::nn::WeightsLoadError::InvalidStandardDeviation);

  auto non_finite_weight = valid;
  write_u32(non_finite_weight, 144, 0x7fc00000U);
  refresh_crc(non_finite_weight);
  EXPECT_EQ(load(std::move(non_finite_weight)), tickbook::nn::WeightsLoadError::NonFiniteParameter);
}

TEST(WeightsLoader, ClearsDestinationModelAfterValidationFailure) {
  auto bytes = fixture_bytes();
  bytes[0] = std::byte{0};
  tickbook::nn::MlpModel model{};
  model.input_mean[0] = 42.0F;
  EXPECT_EQ(tickbook::nn::load_weights(bytes, model), tickbook::nn::WeightsLoadError::WrongMagic);
  EXPECT_EQ(model.input_mean[0], 0.0F);
  EXPECT_EQ(model.output_width(), 0U);
}
