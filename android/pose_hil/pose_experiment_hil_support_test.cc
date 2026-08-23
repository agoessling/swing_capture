#include "android/pose_hil/pose_experiment_hil_support.h"

#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

template <typename Action>
void ExpectInvalid(Action action) {
  try {
    action();
  } catch (const std::invalid_argument &) {
    return;
  }
  assert(false && "expected invalid pose experiment inputs");
}

}  // namespace

int main() {
  namespace pose_hil = swing_capture::android::pose_hil;

  const pose_hil::PoseExperimentHilInputs defaults =
      pose_hil::ResolvePoseExperimentHilInputs("", "");
  assert(defaults.model_variant == "lite");
  assert(defaults.standby_width == 640U);
  assert(defaults.standby_height == 360U);
  assert(pose_hil::IsProductionEquivalent(defaults));

  const pose_hil::PoseExperimentHilInputs heavy =
      pose_hil::ResolvePoseExperimentHilInputs("heavy", "1280x720");
  assert(!pose_hil::IsProductionEquivalent(heavy));
  const std::vector<std::string> expected = {
      "--es", "pose_experiment_model",          "heavy",
      "--ei", "pose_experiment_standby_width",  "1280",
      "--ei", "pose_experiment_standby_height", "720",
  };
  assert(pose_hil::PoseExperimentActivityExtras(heavy) == expected);

  ExpectInvalid(
      [] { static_cast<void>(pose_hil::ResolvePoseExperimentHilInputs("custom", "640x360")); });
  ExpectInvalid(
      [] { static_cast<void>(pose_hil::ResolvePoseExperimentHilInputs("full", "640-360")); });
  ExpectInvalid(
      [] { static_cast<void>(pose_hil::ResolvePoseExperimentHilInputs("full", "639x360")); });
  ExpectInvalid(
      [] { static_cast<void>(pose_hil::ResolvePoseExperimentHilInputs("full", "640x352")); });
  ExpectInvalid(
      [] { static_cast<void>(pose_hil::ResolvePoseExperimentHilInputs("full", "144x90")); });

  pose_hil::PoseExperimentHilInputs constructed = defaults;
  constructed.standby_height = 0;
  ExpectInvalid(
      [&constructed] { static_cast<void>(pose_hil::PoseExperimentActivityExtras(constructed)); });
}
