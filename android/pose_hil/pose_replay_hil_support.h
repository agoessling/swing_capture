#ifndef SWING_CAPTURE_ANDROID_POSE_HIL_POSE_REPLAY_HIL_SUPPORT_H_
#define SWING_CAPTURE_ANDROID_POSE_HIL_POSE_REPLAY_HIL_SUPPORT_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace swing_capture::android::pose_hil {

inline constexpr std::string_view kPoseReplayDeviceDirectory = "files/pose_replay_hil";
inline constexpr std::string_view kPoseReplayDeviceClip = "files/pose_replay_hil/input.mp4";
inline constexpr std::string_view kPoseReplayDeviceReport = "files/reports/latest.json";

struct PoseReplayHilInputs {
  std::string role;
  std::string projection;
  std::string hitting_region;
  std::string delegate_policy;
  std::string expectation;
  std::uint32_t maximum_frames = 0;
};

// Resolves defaults and validates the exact host-to-activity replay contract.
[[nodiscard]] PoseReplayHilInputs ResolvePoseReplayHilInputs(std::string_view role,
                                                             std::string_view projection,
                                                             std::string_view hitting_region,
                                                             std::string_view delegate_policy,
                                                             std::string_view expectation,
                                                             std::string_view maximum_frames);

// Returns the am-start extras consumed by MainActivity. The clip is deliberately
// a fixed basename within the replay-only app-private directory.
[[nodiscard]] std::vector<std::string> PoseReplayActivityExtras(const PoseReplayHilInputs &inputs);

// Fixed run-as shell command that fails until the atomically published report exists. This avoids
// treating Android toybox `cat` diagnostics as JSON while polling.
[[nodiscard]] std::string PoseReplayReportReadShellCommand();

}  // namespace swing_capture::android::pose_hil

#endif  // SWING_CAPTURE_ANDROID_POSE_HIL_POSE_REPLAY_HIL_SUPPORT_H_
