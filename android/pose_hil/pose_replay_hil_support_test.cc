#include "android/pose_hil/pose_replay_hil_support.h"

#include <cassert>
#include <cstdint>
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
  assert(false && "expected invalid replay HIL inputs");
}

}  // namespace

int main() {
  namespace pose_hil = swing_capture::android::pose_hil;

  const pose_hil::PoseReplayHilInputs defaults =
      pose_hil::ResolvePoseReplayHilInputs("", "", "", "", "", "");
  assert(defaults.role == "down_the_line");
  assert(defaults.projection == "dtl");
  assert(defaults.hitting_region == "0.15,0.30,0.85,1.0");
  assert(defaults.delegate_policy == "gpu_preferred");
  assert(defaults.expectation == "observe_only");
  assert(defaults.maximum_frames == 600U);

  const pose_hil::PoseReplayHilInputs face_on = pose_hil::ResolvePoseReplayHilInputs(
      "face_on", "atl", "0.2, 0.3, 0.8, 0.95", "cpu_only", "require_arm", "321");
  assert(face_on.maximum_frames == 321U);
  assert(face_on.hitting_region == "0.2,0.3,0.8,0.95");
  const std::vector<std::string> extras = pose_hil::PoseReplayActivityExtras(face_on);
  const std::vector<std::string> expected = {
      "--ez", "run_pose_replay_hil",        "true",
      "--es", "pose_replay_role",           "face_on",
      "--es", "pose_replay_projection",     "atl",
      "--es", "pose_replay_hitting_region", "0.2,0.3,0.8,0.95",
      "--es", "pose_replay_delegate",       "cpu_only",
      "--es", "pose_replay_expectation",    "require_arm",
      "--ei", "pose_replay_maximum_frames", "321",
      "--es", "pose_replay_clip",           "input.mp4",
  };
  assert(extras == expected);
  assert(pose_hil::PoseReplayReportReadShellCommand() ==
         "test -f files/reports/latest.json && cat files/reports/latest.json");

  ExpectInvalid([] {
    static_cast<void>(pose_hil::ResolvePoseReplayHilInputs("face_on", "dtl", "", "", "", ""));
  });
  ExpectInvalid([] {
    static_cast<void>(pose_hil::ResolvePoseReplayHilInputs("", "", "0.2,0.3,0.2,0.9", "", "", ""));
  });
  ExpectInvalid([] {
    static_cast<void>(pose_hil::ResolvePoseReplayHilInputs("", "", "", "neural", "", ""));
  });
  ExpectInvalid(
      [] { static_cast<void>(pose_hil::ResolvePoseReplayHilInputs("", "", "", "", "", "1201")); });

  pose_hil::PoseReplayHilInputs constructed = defaults;
  constructed.maximum_frames = 0;
  ExpectInvalid(
      [&constructed] { static_cast<void>(pose_hil::PoseReplayActivityExtras(constructed)); });
}
