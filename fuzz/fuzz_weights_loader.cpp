#include <cstddef>
#include <cstdint>
#include <span>

#include "tickbook/nn/mlp.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  tickbook::nn::MlpModel model{};
  const auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size);
  static_cast<void>(tickbook::nn::load_weights(bytes, model));
  return 0;
}
