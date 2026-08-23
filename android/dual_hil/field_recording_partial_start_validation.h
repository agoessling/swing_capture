#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_FIELD_RECORDING_PARTIAL_START_VALIDATION_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_FIELD_RECORDING_PARTIAL_START_VALIDATION_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace swing_capture::android::dual_hil {

inline constexpr std::string_view kFieldRecordingHilRejection =
    "HIL injected field-recording start rejection";

struct FieldRecordingPartialStartEvidence {
  std::string rejected_recording_id;
  std::string retry_recording_id;
  int down_initial_start_status = 0;
  int face_initial_start_status = 0;
  std::string face_initial_error;
  bool face_fault_armed_before_start = false;
  bool face_fault_consumed_after_rejection = false;
  int down_rollback_stop_status = 0;
  std::string down_rollback_state;
  std::string down_rollback_recording_id;
  std::string face_after_rejection_state;
  std::string face_after_rejection_recording_id;
  int down_retry_start_status = 0;
  int face_retry_start_status = 0;
  std::string down_retry_recording_state;
  std::string down_retry_recording_id;
  std::string face_retry_recording_state;
  std::string face_retry_recording_id;
  int down_retry_stop_status = 0;
  int face_retry_stop_status = 0;
  std::string down_terminal_state;
  std::string down_terminal_recording_id;
  std::string face_terminal_state;
  std::string face_terminal_recording_id;
  bool cleanup_passed = false;
  std::int64_t elapsed_ms = 0;
};

void ValidateFieldRecordingPartialStart(const FieldRecordingPartialStartEvidence &evidence);

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_FIELD_RECORDING_PARTIAL_START_VALIDATION_H_
