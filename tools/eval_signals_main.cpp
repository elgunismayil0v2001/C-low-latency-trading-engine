#include <exception>
#include <iostream>

#include "eval_signals.hpp"

int main(int argc, char** argv) {
  try {
    if (argc != 2) {
      std::cerr << "Usage: eval_signals FEATURES.tbf\n";
      return 2;
    }
    const auto result = tickbook::signal_eval::evaluate_feature_file(argv[1]);
    if (!result.success) {
      std::cerr << "eval_signals: " << result.error << '\n';
      return 1;
    }
    std::cout << result.report;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "eval_signals: " << error.what() << '\n';
    return 1;
  }
}
