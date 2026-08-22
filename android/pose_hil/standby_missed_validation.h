#ifndef SWING_CAPTURE_ANDROID_POSE_HIL_STANDBY_MISSED_VALIDATION_H_
#define SWING_CAPTURE_ANDROID_POSE_HIL_STANDBY_MISSED_VALIDATION_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace swing_capture::android::pose_hil {

enum class StandbyDiagnosticExpectation {
  kOperatorMissedShot,
  kAutomaticImpact,
};

struct StandbyMissedInspection {
  std::string_view manifest;
  std::string_view diagnostic_audio_wav;
  std::string_view diagnostic_incident;
  std::string_view preview_mjpeg;
  std::string_view pose_trace_ndjson;
  std::string_view diagnostics_zip;
  std::string_view expected_session_id;
  StandbyDiagnosticExpectation expectation = StandbyDiagnosticExpectation::kOperatorMissedShot;
};

struct StandbyMissedEvidence {
  bool valid = false;
  std::string diagnostic;
  std::size_t wav_bytes = 0;
  std::size_t sample_count = 0;
  std::size_t pre_roll_frames = 0;
  std::size_t post_roll_frames = 0;
  bool startup_short_pre_roll = false;
  std::size_t preview_frame_count = 0;
  std::size_t zip_bytes = 0;
  std::size_t zip_entry_count = 0;
};

// Validates one operator-tagged standby diagnostic session and its transport archive.
[[nodiscard]] StandbyMissedEvidence ValidateStandbyMissedEvidence(
    const StandbyMissedInspection &inspection) noexcept;

}  // namespace swing_capture::android::pose_hil

#endif  // SWING_CAPTURE_ANDROID_POSE_HIL_STANDBY_MISSED_VALIDATION_H_
