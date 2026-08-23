#include <cstddef>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>  // NOLINT(misc-include-cleaner)
#include <span>
#include <stdexcept>
#include <string>

#include "android/dual_hil/timing_calibration_evidence.h"

int main(int argument_count, char **arguments) {
  try {
    const auto command_line = std::span(arguments, static_cast<std::size_t>(argument_count));
    if (command_line.size() != 3U) {
      throw std::invalid_argument("usage: timing_calibration_score EVIDENCE.json ARTIFACT_ROOT");
    }
    std::ifstream input(command_line[1], std::ios::binary);
    if (!input) {
      throw std::runtime_error("cannot open timing calibration evidence");
    }
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    std::cout << swing_capture::android::dual_hil::EvaluateTimingCalibrationEvidence(
                     contents, command_line[2])
                     .dump(2)
              << '\n';
    return 0;
  } catch (const std::exception &failure) {
    std::cerr << failure.what() << '\n';
    return 2;
  }
}
