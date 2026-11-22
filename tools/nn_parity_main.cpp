#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "tickbook/nn/mlp.hpp"

namespace {

constexpr std::size_t kInputRecordSize = tickbook::nn::kMlpFeatureCount * sizeof(std::int64_t);

[[nodiscard]] std::uint64_t read_u64(const std::byte* bytes) noexcept {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[index]))
             << (8U * index);
  }
  return value;
}

[[nodiscard]] std::vector<std::byte> read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open '" + path.string() + "'");
  std::vector<char> chars((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes(chars.size());
  for (std::size_t index = 0; index < chars.size(); ++index) {
    bytes[index] = static_cast<std::byte>(static_cast<unsigned char>(chars[index]));
  }
  return bytes;
}

[[nodiscard]] tickbook::FeatureVector decode_features(const std::byte* bytes) {
  std::array<std::int64_t, tickbook::nn::kMlpFeatureCount> raw{};
  for (std::size_t index = 0; index < raw.size(); ++index) {
    raw[index] = std::bit_cast<std::int64_t>(read_u64(bytes + index * sizeof(std::int64_t)));
  }
  if (raw[1] < INT32_MIN || raw[1] > INT32_MAX || raw[2] < INT32_MIN || raw[2] > INT32_MAX) {
    throw std::runtime_error("feature vector has an out-of-range Q20 imbalance");
  }
  tickbook::FeatureVector result{};
  result.spread_ticks = raw[0];
  result.imbalance_l1_q20 = static_cast<std::int32_t>(raw[1]);
  result.imbalance_k_q20 = static_cast<std::int32_t>(raw[2]);
  result.microprice_offset_q20_ticks = raw[3];
  result.ofi_10 = raw[4];
  result.ofi_50 = raw[5];
  result.ofi_200 = raw[6];
  result.trade_flow_100ms = raw[7];
  result.trade_flow_1s = raw[8];
  result.mid_return_100ms_q20_ticks = raw[9];
  result.mid_return_1s_q20_ticks = raw[10];
  return result;
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 3) {
      std::cerr << "Usage: nn_parity WEIGHTS.bin INPUT_FEATURES.bin\n";
      return 2;
    }
    auto weights_bytes = read_file(argv[1]);
    tickbook::nn::MlpModel model{};
    const auto load_error = tickbook::nn::load_weights(weights_bytes, model);
    if (load_error != tickbook::nn::WeightsLoadError::None) {
      std::cerr << "nn_parity: " << tickbook::nn::weights_load_error_message(load_error) << '\n';
      return 1;
    }
    const auto input_bytes = read_file(argv[2]);
    if (input_bytes.size() % kInputRecordSize != 0) {
      std::cerr << "nn_parity: input feature file has a partial record\n";
      return 1;
    }
    for (std::size_t offset = 0; offset < input_bytes.size(); offset += kInputRecordSize) {
      const auto features = decode_features(input_bytes.data() + offset);
      const auto result = model.infer(features);
      std::cout.write(reinterpret_cast<const char*>(result.values.data()),
                      static_cast<std::streamsize>(result.count * sizeof(float)));
      if (!std::cout) {
        std::cerr << "nn_parity: cannot write inference results\n";
        return 1;
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "nn_parity: " << error.what() << '\n';
    return 1;
  }
}
