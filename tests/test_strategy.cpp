#include <gtest/gtest.h>

#include <array>

#include "tickbook/nn/signal.hpp"
#include "tickbook/sim/strategy.hpp"

TEST(ThresholdStrategy, TradesOnlyPastThresholdAndRespectsProjectedLongPosition) {
  constexpr auto kQ20 = std::int64_t{1} << 20U;
  tickbook::sim::ThresholdStrategy strategy({2, 5}, kQ20 / 4);
  tickbook::sim::BookView book{};

  EXPECT_FALSE(strategy.on_signal({kQ20 / 8, tickbook::sim::SignalKind::FeatureImbalance}, book));
  const auto buy =
      strategy.on_signal({kQ20 / 2, tickbook::sim::SignalKind::FeatureImbalance}, book);
  EXPECT_EQ(buy.side, tickbook::sim::OrderSide::Buy);
  EXPECT_EQ(buy.qty, 2);

  book.projected_position = 4;
  EXPECT_EQ(strategy.on_signal({kQ20, tickbook::sim::SignalKind::FeatureImbalance}, book).qty, 1);
  book.projected_position = 5;
  EXPECT_FALSE(strategy.on_signal({kQ20, tickbook::sim::SignalKind::FeatureImbalance}, book));

  book.projected_position = 3;
  const auto sell = strategy.on_signal({-kQ20, tickbook::sim::SignalKind::FeatureImbalance}, book);
  EXPECT_EQ(sell.side, tickbook::sim::OrderSide::Sell);
  EXPECT_EQ(sell.qty, 2);
  book.projected_position = 0;
  EXPECT_FALSE(strategy.on_signal({-kQ20, tickbook::sim::SignalKind::FeatureImbalance}, book));
}

TEST(ThresholdStrategy, DoesNotTradeNeutralScoresOrSignalsFromAnotherSource) {
  tickbook::sim::ThresholdStrategy strategy({1, 1}, 0);
  tickbook::sim::BookView book{};
  EXPECT_FALSE(strategy.on_signal({0, tickbook::sim::SignalKind::FeatureImbalance}, book));
  EXPECT_FALSE(strategy.on_signal({1, tickbook::sim::SignalKind::NeuralReturn}, book));
}

TEST(NnStrategy, UsesSeparateReturnAndDirectionThresholds) {
  constexpr auto kQ20 = std::int64_t{1} << 20U;
  tickbook::sim::NnStrategy strategy({1, 2}, kQ20, kQ20 / 2);
  tickbook::sim::BookView book{};
  EXPECT_FALSE(strategy.on_signal({kQ20 / 2, tickbook::sim::SignalKind::NeuralReturn}, book));
  EXPECT_EQ(strategy.on_signal({2 * kQ20, tickbook::sim::SignalKind::NeuralReturn}, book).side,
            tickbook::sim::OrderSide::Buy);
  EXPECT_FALSE(strategy.on_signal({kQ20 / 4, tickbook::sim::SignalKind::NeuralDirection}, book));
  EXPECT_EQ(strategy.on_signal({kQ20, tickbook::sim::SignalKind::NeuralDirection}, book).qty, 1);
}

TEST(NnSignal, QuantizesOutputsAndMapsDirectionMarginToASignedSignal) {
  tickbook::nn::MlpModel model{};
  model.layers = {{{11, 16, tickbook::nn::Activation::Relu},
                   {16, 16, tickbook::nn::Activation::Relu},
                   {16, 3, tickbook::nn::Activation::Linear}}};
  model.input_standard_deviation.fill(1.0F);
  model.input_inverse_standard_deviation.fill(1.0F);
  model.biases[2][2] = 1.0F;
  model.prepare_vector_weights();

  const auto positive = tickbook::nn::make_signal(model, {});
  EXPECT_EQ(positive.kind, tickbook::sim::SignalKind::NeuralDirection);
  EXPECT_EQ(positive.score_q20, std::int64_t{1} << 20U);

  model.biases[2].fill(0.0F);
  model.biases[2][0] = 2.0F;
  model.biases[2][1] = 1.0F;
  model.prepare_vector_weights();
  const auto negative = tickbook::nn::make_signal(model, {});
  EXPECT_EQ(negative.score_q20, -(std::int64_t{1} << 20U));
}
