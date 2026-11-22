#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "tickbook/nn/mlp.hpp"
#include "tickbook/sim/strategy.hpp"

namespace tickbook::nn {

inline constexpr std::int64_t kSignalQ20 = 1'048'576;

// Inference is the only boundary where model floats return to the fixed-point
// engine. Saturation keeps a malformed but finite model output out of UB.
[[nodiscard]] inline std::int64_t output_to_q20(float value) noexcept {
  if (!std::isfinite(value)) return 0;
  const auto scaled = static_cast<double>(value) * static_cast<double>(kSignalQ20);
  constexpr double kInt64Limit = 9'223'372'036'854'775'808.0;
  if (scaled >= kInt64Limit) return std::numeric_limits<std::int64_t>::max();
  if (scaled <= -kInt64Limit) return std::numeric_limits<std::int64_t>::min();
  return static_cast<std::int64_t>(scaled);
}

[[nodiscard]] inline sim::Signal make_signal(const MlpModel& model,
                                             const FeatureVector& features) noexcept {
  const auto output = model.infer(features);
  if (output.count == 1) {
    return {output_to_q20(output.values[0]), sim::SignalKind::NeuralReturn};
  }
  if (output.count != 3) return {};

  std::size_t best = 0;
  std::size_t second = 1;
  if (output.values[second] > output.values[best]) std::swap(best, second);
  for (std::size_t index = 2; index < output.count; ++index) {
    if (output.values[index] > output.values[best]) {
      second = best;
      best = index;
    } else if (output.values[index] > output.values[second]) {
      second = index;
    }
  }
  if (best == 1) return {0, sim::SignalKind::NeuralDirection};
  const auto margin = output_to_q20(output.values[best] - output.values[second]);
  return {best == 2 ? margin : -margin, sim::SignalKind::NeuralDirection};
}

} // namespace tickbook::nn
