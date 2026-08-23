#include "android/pose_hil/pose_replay_hil_support.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace swing_capture::android::pose_hil {
namespace {

std::array<double, 4> ParseHittingRegion(std::string_view text) {
  std::array<double, 4> values = {};
  for (std::size_t index = 0; index < values.size(); ++index) {
    const std::size_t separator = text.find(',');
    if ((index + 1U < values.size()) != (separator != std::string_view::npos)) {
      throw std::invalid_argument("pose hitting region must contain four coordinates");
    }
    std::string_view field = text.substr(0, separator);
    while (!field.empty() && (field.front() == ' ' || field.front() == '\t')) {
      field.remove_prefix(1);
    }
    while (!field.empty() && (field.back() == ' ' || field.back() == '\t')) {
      field.remove_suffix(1);
    }
    const auto [end, error] = std::from_chars(field.begin(), field.end(), values[index]);
    if (error != std::errc() || end != field.end() || !std::isfinite(values[index]) ||
        values[index] < 0.0 || values[index] > 1.0) {
      throw std::invalid_argument("pose hitting region coordinates must be finite and in [0,1]");
    }
    text = separator == std::string_view::npos ? std::string_view() : text.substr(separator + 1U);
  }
  if (values[2] <= values[0] || values[3] <= values[1]) {
    throw std::invalid_argument("pose hitting region must have positive area");
  }
  return values;
}

std::uint32_t ParseMaximumFrames(std::string_view text) {
  if (text.empty()) {
    return 600U;
  }
  std::uint32_t value = 0;
  const auto [end, error] = std::from_chars(text.begin(), text.end(), value);
  if (error != std::errc() || end != text.end() || value == 0U || value > 1200U) {
    throw std::invalid_argument("pose replay maximum frames must be in [1,1200]");
  }
  return value;
}

}  // namespace

PoseReplayHilInputs ResolvePoseReplayHilInputs(
    std::string_view role, std::string_view projection, std::string_view hitting_region,
    std::string_view delegate_policy,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    std::string_view expectation, std::string_view maximum_frames) {
  PoseReplayHilInputs inputs;
  inputs.role = role.empty() ? "down_the_line" : std::string(role);
  if (inputs.role != "down_the_line" && inputs.role != "face_on") {
    throw std::invalid_argument("pose replay role must be down_the_line or face_on");
  }
  if (projection.empty()) {
    inputs.projection = inputs.role == "down_the_line" ? "dtl" : "atl";
  } else {
    inputs.projection = projection;
  }
  if ((inputs.role == "down_the_line" && inputs.projection != "dtl") ||
      (inputs.role == "face_on" && inputs.projection != "atl")) {
    throw std::invalid_argument("pose replay projection must match the configured role");
  }
  inputs.hitting_region =
      hitting_region.empty() ? "0.15,0.30,0.85,1.0" : std::string(hitting_region);
  if (inputs.hitting_region.size() > 128U) {
    throw std::invalid_argument("pose hitting region exceeds its input bound");
  }
  static_cast<void>(ParseHittingRegion(inputs.hitting_region));
  std::string compact_region;
  compact_region.reserve(inputs.hitting_region.size());
  for (const char character : inputs.hitting_region) {
    if (character != ' ' && character != '\t') {
      compact_region.push_back(character);
    }
  }
  inputs.hitting_region = compact_region;
  inputs.delegate_policy = delegate_policy.empty() ? "gpu_preferred" : std::string(delegate_policy);
  if (inputs.delegate_policy != "cpu_only" && inputs.delegate_policy != "gpu_preferred" &&
      inputs.delegate_policy != "gpu_required" && inputs.delegate_policy != "npu_preferred" &&
      inputs.delegate_policy != "npu_required") {
    throw std::invalid_argument(
        "pose replay delegate must be cpu_only, gpu_preferred, gpu_required, npu_preferred, or "
        "npu_required");
  }
  inputs.expectation = expectation.empty() ? "observe_only" : std::string(expectation);
  if (inputs.expectation != "observe_only" && inputs.expectation != "require_arm" &&
      inputs.expectation != "require_no_arm") {
    throw std::invalid_argument(
        "pose replay expectation must be observe_only, require_arm, or require_no_arm");
  }
  inputs.maximum_frames = ParseMaximumFrames(maximum_frames);
  return inputs;
}

std::vector<std::string> PoseReplayActivityExtras(const PoseReplayHilInputs &inputs) {
  // Revalidate caller-constructed records before they cross the adb boundary.
  const PoseReplayHilInputs validated = ResolvePoseReplayHilInputs(
      inputs.role, inputs.projection, inputs.hitting_region, inputs.delegate_policy,
      inputs.expectation, std::to_string(inputs.maximum_frames));
  return {
      "--ez", "run_pose_replay_hil",        "true",
      "--es", "pose_replay_role",           validated.role,
      "--es", "pose_replay_projection",     validated.projection,
      "--es", "pose_replay_hitting_region", validated.hitting_region,
      "--es", "pose_replay_delegate",       validated.delegate_policy,
      "--es", "pose_replay_expectation",    validated.expectation,
      "--ei", "pose_replay_maximum_frames", std::to_string(validated.maximum_frames),
      "--es", "pose_replay_clip",           "input.mp4",
  };
}

std::string PoseReplayReportReadShellCommand() {
  return "test -f files/reports/latest.json && cat files/reports/latest.json";
}

}  // namespace swing_capture::android::pose_hil
