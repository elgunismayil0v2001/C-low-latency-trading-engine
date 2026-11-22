#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

#include "tickbook/features/feature_engine.hpp"

namespace tickbook::nn {

inline constexpr std::size_t kMlpFeatureCount = 11;
inline constexpr std::size_t kMlpHiddenWidth = 16;
inline constexpr std::size_t kMlpLayerCount = 3;
inline constexpr std::size_t kMlpMaximumOutputWidth = 3;
inline constexpr std::size_t kMlpMaximumLayerWeights = kMlpHiddenWidth * kMlpHiddenWidth;
inline constexpr std::uint32_t kWeightsVersion = 1;
inline constexpr std::uint32_t kWeightsEndianness = 0x01020304U;
inline constexpr std::array<char, 8> kWeightsMagic{'T', 'I', 'C', 'K', 'N', 'N', '0', '1'};

enum class Activation : std::uint8_t {
  Linear = 0,
  Relu = 1,
};

struct LayerDescriptor {
  std::uint32_t input_width{};
  std::uint32_t output_width{};
  Activation activation{Activation::Linear};
};

struct InferenceOutput {
  std::array<float, kMlpMaximumOutputWidth> values{};
  std::uint32_t count{};
};

// This is the only fixed-point-to-float conversion in the engine. Normalization uses
// training-split statistics stored beside the scalar weights.
inline void features_to_float(const FeatureVector& features,
                              const std::array<float, kMlpFeatureCount>& mean,
                              const std::array<float, kMlpFeatureCount>& inverse_standard_deviation,
                              std::array<float, kMlpFeatureCount>& normalized) noexcept {
  const auto normalize = [&](std::int64_t value, std::size_t index) noexcept {
    return (static_cast<float>(value) - mean[index]) * inverse_standard_deviation[index];
  };
  normalized = {normalize(features.spread_ticks, 0),
                normalize(features.imbalance_l1_q20, 1),
                normalize(features.imbalance_k_q20, 2),
                normalize(features.microprice_offset_q20_ticks, 3),
                normalize(features.ofi_10, 4),
                normalize(features.ofi_50, 5),
                normalize(features.ofi_200, 6),
                normalize(features.trade_flow_100ms, 7),
                normalize(features.trade_flow_1s, 8),
                normalize(features.mid_return_100ms_q20_ticks, 9),
                normalize(features.mid_return_1s_q20_ticks, 10)};
}

struct MlpModel {
  std::array<float, kMlpFeatureCount> input_mean{};
  std::array<float, kMlpFeatureCount> input_standard_deviation{};
  std::array<float, kMlpFeatureCount> input_inverse_standard_deviation{};
  std::array<LayerDescriptor, kMlpLayerCount> layers{};
  std::array<std::array<float, kMlpMaximumLayerWeights>, kMlpLayerCount> weights{};
  std::array<std::array<float, kMlpMaximumLayerWeights>, kMlpLayerCount> vector_weights{};
  std::array<std::array<float, kMlpHiddenWidth>, kMlpLayerCount> biases{};

  [[nodiscard]] std::uint32_t output_width() const noexcept {
    return layers[kMlpLayerCount - 1].output_width;
  }

  [[nodiscard]] InferenceOutput infer(const FeatureVector& features) const noexcept {
#if defined(__ARM_NEON) || defined(__SSE2__)
    return infer_vectorized(features);
#else
    return infer_scalar(features);
#endif
  }

  [[nodiscard]] InferenceOutput infer_scalar(const FeatureVector& features) const noexcept {
    std::array<float, kMlpFeatureCount> input{};
    features_to_float(features, input_mean, input_inverse_standard_deviation, input);
    std::array<float, kMlpHiddenWidth> first{};
    std::array<float, kMlpHiddenWidth> second{};
    const float* current = input.data();
    for (std::size_t layer_index = 0; layer_index < kMlpLayerCount; ++layer_index) {
      const auto& layer = layers[layer_index];
      auto& destination = layer_index % 2U == 0 ? first : second;
      const auto& layer_weights = weights[layer_index];
      const auto& layer_biases = biases[layer_index];
      for (std::size_t output = 0; output < layer.output_width; ++output) {
        float value = layer_biases[output];
        const auto offset = output * layer.input_width;
        for (std::size_t input_index = 0; input_index < layer.input_width; ++input_index) {
          value += layer_weights[offset + input_index] * current[input_index];
        }
        if (layer.activation == Activation::Relu && value < 0.0F) value = 0.0F;
        destination[output] = value;
      }
      current = destination.data();
    }
    InferenceOutput result{};
    result.count = output_width();
    for (std::size_t index = 0; index < result.count; ++index) result.values[index] = first[index];
    return result;
  }

  void prepare_vector_weights() noexcept {
    for (std::size_t layer_index = 0; layer_index < layers.size(); ++layer_index) {
      auto& destination = vector_weights[layer_index];
      destination.fill(0.0F);
      const auto& layer = layers[layer_index];
      const auto& source = weights[layer_index];
      for (std::size_t input_index = 0; input_index < layer.input_width; ++input_index) {
        for (std::size_t output = 0; output < layer.output_width; ++output) {
          destination[input_index * kMlpHiddenWidth + output] =
              source[output * layer.input_width + input_index];
        }
      }
    }
  }

private:
  struct VectorLayerData {
    const float* weights{};
    const float* biases{};
  };

  [[nodiscard]] InferenceOutput infer_vectorized(const FeatureVector& features) const noexcept {
    std::array<float, kMlpFeatureCount> input{};
    features_to_float(features, input_mean, input_inverse_standard_deviation, input);
    std::array<float, kMlpHiddenWidth> first{};
    std::array<float, kMlpHiddenWidth> second{};
    vector_layer<kMlpFeatureCount, kMlpHiddenWidth, true>(
        input.data(), first.data(), VectorLayerData{vector_weights[0].data(), biases[0].data()});
    vector_layer<kMlpHiddenWidth, kMlpHiddenWidth, true>(
        first.data(), second.data(), VectorLayerData{vector_weights[1].data(), biases[1].data()});
    if (output_width() == 1) {
      vector_layer<kMlpHiddenWidth, 1, false>(
          second.data(), first.data(), VectorLayerData{vector_weights[2].data(), biases[2].data()});
    } else {
      vector_layer<kMlpHiddenWidth, 3, false>(
          second.data(), first.data(), VectorLayerData{vector_weights[2].data(), biases[2].data()});
    }
    InferenceOutput result{};
    result.count = output_width();
    for (std::size_t index = 0; index < result.count; ++index) result.values[index] = first[index];
    return result;
  }

  template <std::size_t InputWidth, std::size_t OutputWidth, bool Relu>
  static void vector_layer(const float* input, float* output,
                           const VectorLayerData& parameters) noexcept {
    const auto* weights = parameters.weights;
    const auto* biases = parameters.biases;
    for (std::size_t output_index = 0; output_index < OutputWidth; output_index += 4) {
#if defined(__ARM_NEON)
      auto sum = vdupq_n_f32(0.0F);
      for (std::size_t input_index = 0; input_index < InputWidth; ++input_index) {
        sum = vfmaq_n_f32(sum, vld1q_f32(weights + input_index * kMlpHiddenWidth + output_index),
                          input[input_index]);
      }
      auto value = vaddq_f32(sum, vld1q_f32(biases + output_index));
      if constexpr (Relu) value = vmaxq_f32(value, vdupq_n_f32(0.0F));
      vst1q_f32(output + output_index, value);
#elif defined(__SSE2__)
      auto sum = _mm_setzero_ps();
      for (std::size_t input_index = 0; input_index < InputWidth; ++input_index) {
        const auto row = weights + input_index * kMlpHiddenWidth + output_index;
        sum = _mm_add_ps(sum, _mm_mul_ps(_mm_loadu_ps(row), _mm_set1_ps(input[input_index])));
      }
      auto value = _mm_add_ps(sum, _mm_loadu_ps(biases + output_index));
      if constexpr (Relu) value = _mm_max_ps(value, _mm_setzero_ps());
      _mm_storeu_ps(output + output_index, value);
#endif
    }
  }
};

enum class WeightsLoadError : std::uint8_t {
  None,
  HeaderTooSmall,
  WrongMagic,
  UnsupportedVersion,
  WrongEndianness,
  WrongLayerCount,
  InvalidDimensions,
  InvalidActivation,
  SizeMismatch,
  ChecksumMismatch,
  NonFiniteNormalization,
  InvalidStandardDeviation,
  NonFiniteParameter,
};

namespace weights_detail {

[[nodiscard]] inline std::uint32_t read_u32(std::span<const std::byte> bytes,
                                            std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index]))
             << (8U * index);
  }
  return value;
}

[[nodiscard]] inline float read_float(std::span<const std::byte> bytes,
                                      std::size_t offset) noexcept {
  return std::bit_cast<float>(read_u32(bytes, offset));
}

[[nodiscard]] inline std::uint32_t crc32(std::span<const std::byte> bytes) noexcept {
  std::uint32_t crc = 0xffffffffU;
  for (const auto byte : bytes) {
    crc ^= std::to_integer<std::uint8_t>(byte);
    for (std::size_t bit = 0; bit < 8; ++bit) {
      const auto mask = static_cast<std::uint32_t>(0U - (crc & 1U));
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return ~crc;
}

} // namespace weights_detail

[[nodiscard]] inline WeightsLoadError load_weights(std::span<const std::byte> bytes,
                                                   MlpModel& output) noexcept {
  output = {};
  constexpr std::size_t kFixedHeaderSize = 20;
  constexpr std::size_t kLayerDescriptorSize = 12;
  constexpr std::size_t kNormalizationBytes = kMlpFeatureCount * sizeof(float) * 2;
  constexpr std::size_t kChecksumBytes = sizeof(std::uint32_t);
  if (bytes.size() <
      kFixedHeaderSize + kLayerDescriptorSize + kNormalizationBytes + kChecksumBytes) {
    return WeightsLoadError::HeaderTooSmall;
  }
  for (std::size_t index = 0; index < kWeightsMagic.size(); ++index) {
    if (bytes[index] != static_cast<std::byte>(static_cast<unsigned char>(kWeightsMagic[index]))) {
      return WeightsLoadError::WrongMagic;
    }
  }
  if (weights_detail::read_u32(bytes, 8) != kWeightsVersion) {
    return WeightsLoadError::UnsupportedVersion;
  }
  if (weights_detail::read_u32(bytes, 12) != kWeightsEndianness) {
    return WeightsLoadError::WrongEndianness;
  }
  if (weights_detail::read_u32(bytes, 16) != kMlpLayerCount) {
    return WeightsLoadError::WrongLayerCount;
  }

  MlpModel candidate{};
  std::size_t cursor = kFixedHeaderSize;
  for (std::size_t index = 0; index < candidate.layers.size(); ++index) {
    auto& descriptor = candidate.layers[index];
    descriptor.input_width = weights_detail::read_u32(bytes, cursor);
    descriptor.output_width = weights_detail::read_u32(bytes, cursor + 4);
    const auto activation = weights_detail::read_u32(bytes, cursor + 8);
    if (activation > static_cast<std::uint32_t>(Activation::Relu)) {
      return WeightsLoadError::InvalidActivation;
    }
    descriptor.activation = static_cast<Activation>(activation);
    cursor += kLayerDescriptorSize;
  }
  const auto valid_dimensions = candidate.layers[0].input_width == kMlpFeatureCount &&
                                candidate.layers[0].output_width == kMlpHiddenWidth &&
                                candidate.layers[1].input_width == kMlpHiddenWidth &&
                                candidate.layers[1].output_width == kMlpHiddenWidth &&
                                candidate.layers[2].input_width == kMlpHiddenWidth &&
                                (candidate.layers[2].output_width == 1 ||
                                 candidate.layers[2].output_width == kMlpMaximumOutputWidth) &&
                                candidate.layers[0].activation == Activation::Relu &&
                                candidate.layers[1].activation == Activation::Relu &&
                                candidate.layers[2].activation == Activation::Linear;
  if (!valid_dimensions) return WeightsLoadError::InvalidDimensions;

  const auto parameter_start = cursor + kNormalizationBytes;
  std::size_t expected_size = parameter_start + kChecksumBytes;
  for (const auto& descriptor : candidate.layers) {
    const auto parameter_count =
        static_cast<std::size_t>(descriptor.input_width) * descriptor.output_width +
        descriptor.output_width;
    expected_size += parameter_count * sizeof(float);
  }
  if (bytes.size() != expected_size) return WeightsLoadError::SizeMismatch;

  for (std::size_t index = 0; index < kMlpFeatureCount; ++index) {
    candidate.input_mean[index] = weights_detail::read_float(bytes, cursor + index * sizeof(float));
    if (!std::isfinite(candidate.input_mean[index])) {
      return WeightsLoadError::NonFiniteNormalization;
    }
  }
  cursor += kMlpFeatureCount * sizeof(float);
  for (std::size_t index = 0; index < kMlpFeatureCount; ++index) {
    candidate.input_standard_deviation[index] =
        weights_detail::read_float(bytes, cursor + index * sizeof(float));
    if (!std::isfinite(candidate.input_standard_deviation[index])) {
      return WeightsLoadError::NonFiniteNormalization;
    }
    if (candidate.input_standard_deviation[index] <= 0.0F) {
      return WeightsLoadError::InvalidStandardDeviation;
    }
    candidate.input_inverse_standard_deviation[index] =
        1.0F / candidate.input_standard_deviation[index];
    if (!std::isfinite(candidate.input_inverse_standard_deviation[index])) {
      return WeightsLoadError::InvalidStandardDeviation;
    }
  }
  cursor += kMlpFeatureCount * sizeof(float);

  for (std::size_t layer_index = 0; layer_index < candidate.layers.size(); ++layer_index) {
    const auto& descriptor = candidate.layers[layer_index];
    const auto weight_count =
        static_cast<std::size_t>(descriptor.input_width) * descriptor.output_width;
    auto& layer_weights = candidate.weights[layer_index];
    auto& layer_biases = candidate.biases[layer_index];
    for (std::size_t index = 0; index < weight_count; ++index) {
      layer_weights[index] = weights_detail::read_float(bytes, cursor);
      if (!std::isfinite(layer_weights[index])) return WeightsLoadError::NonFiniteParameter;
      cursor += sizeof(float);
    }
    for (std::size_t index = 0; index < descriptor.output_width; ++index) {
      layer_biases[index] = weights_detail::read_float(bytes, cursor);
      if (!std::isfinite(layer_biases[index])) return WeightsLoadError::NonFiniteParameter;
      cursor += sizeof(float);
    }
  }
  const auto stored_crc = weights_detail::read_u32(bytes, bytes.size() - kChecksumBytes);
  if (stored_crc != weights_detail::crc32(bytes.first(bytes.size() - kChecksumBytes))) {
    return WeightsLoadError::ChecksumMismatch;
  }
  candidate.prepare_vector_weights();
  output = candidate;
  return WeightsLoadError::None;
}

[[nodiscard]] inline const char* weights_load_error_message(WeightsLoadError error) noexcept {
  switch (error) {
  case WeightsLoadError::None: return "no error";
  case WeightsLoadError::HeaderTooSmall: return "weights file is truncated";
  case WeightsLoadError::WrongMagic: return "weights magic is invalid";
  case WeightsLoadError::UnsupportedVersion: return "weights version is unsupported";
  case WeightsLoadError::WrongEndianness: return "weights endianness marker is invalid";
  case WeightsLoadError::WrongLayerCount: return "weights layer count is invalid";
  case WeightsLoadError::InvalidDimensions:
    return "weights dimensions or activations do not match 11-16-16-output MLP";
  case WeightsLoadError::InvalidActivation: return "weights activation id is unsupported";
  case WeightsLoadError::SizeMismatch: return "weights file size does not match its dimensions";
  case WeightsLoadError::ChecksumMismatch: return "weights CRC32 does not match";
  case WeightsLoadError::NonFiniteNormalization:
    return "weights normalization contains a non-finite value";
  case WeightsLoadError::InvalidStandardDeviation:
    return "weights standard deviation must be positive";
  case WeightsLoadError::NonFiniteParameter: return "weights parameters contain a non-finite value";
  }
  return "unknown weights error";
}

} // namespace tickbook::nn
