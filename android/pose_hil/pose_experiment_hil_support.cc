#include "android/pose_hil/pose_experiment_hil_support.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace swing_capture::android::pose_hil {
namespace {

constexpr std::uint32_t kMinimumWidth = 160;
constexpr std::uint32_t kMaximumWidth = 1280;
constexpr std::uint32_t kMinimumHeight = 90;
constexpr std::uint32_t kMaximumHeight = 720;

std::uint32_t ParseDimension(std::string_view field) {
  std::uint32_t value = 0;
  const auto [end, error] = std::from_chars(field.begin(), field.end(), value);
  if (error != std::errc() || end != field.end()) {
    throw std::invalid_argument("pose experiment standby size must be WIDTHxHEIGHT");
  }
  return value;
}

void Validate(const PoseExperimentHilInputs &inputs) {
  if (inputs.model_variant != "lite" && inputs.model_variant != "full" &&
      inputs.model_variant != "heavy") {
    throw std::invalid_argument("pose experiment model must be lite, full, or heavy");
  }
  if (inputs.standby_width < kMinimumWidth || inputs.standby_width > kMaximumWidth ||
      inputs.standby_height < kMinimumHeight || inputs.standby_height > kMaximumHeight ||
      inputs.standby_width % 2U != 0U || inputs.standby_height % 2U != 0U ||
      static_cast<std::uint64_t>(inputs.standby_width) * 9U !=
          static_cast<std::uint64_t>(inputs.standby_height) * 16U) {
    throw std::invalid_argument(
        "pose experiment standby input must be an even 16:9 size from 160x90 through 1280x720");
  }
}

}  // namespace

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
PoseExperimentHilInputs ResolvePoseExperimentHilInputs(std::string_view model_variant,
                                                       std::string_view standby_size) {
  PoseExperimentHilInputs inputs;
  inputs.model_variant = model_variant.empty() ? "lite" : std::string(model_variant);
  const std::string_view size = standby_size.empty() ? std::string_view("640x360") : standby_size;
  const std::size_t separator = size.find('x');
  if (separator == std::string_view::npos ||
      size.find('x', separator + 1U) != std::string_view::npos) {
    throw std::invalid_argument("pose experiment standby size must be WIDTHxHEIGHT");
  }
  inputs.standby_width = ParseDimension(size.substr(0, separator));
  inputs.standby_height = ParseDimension(size.substr(separator + 1U));
  Validate(inputs);
  return inputs;
}

std::vector<std::string> PoseExperimentActivityExtras(const PoseExperimentHilInputs &inputs) {
  Validate(inputs);
  return {
      "--es", "pose_experiment_model",          inputs.model_variant,
      "--ei", "pose_experiment_standby_width",  std::to_string(inputs.standby_width),
      "--ei", "pose_experiment_standby_height", std::to_string(inputs.standby_height),
  };
}

bool IsProductionEquivalent(const PoseExperimentHilInputs &inputs) {
  Validate(inputs);
  return inputs.model_variant == "lite" && inputs.standby_width == 640U &&
         inputs.standby_height == 360U;
}

}  // namespace swing_capture::android::pose_hil
