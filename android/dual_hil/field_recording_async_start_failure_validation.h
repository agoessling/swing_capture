#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_FIELD_RECORDING_ASYNC_START_FAILURE_VALIDATION_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_FIELD_RECORDING_ASYNC_START_FAILURE_VALIDATION_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace swing_capture::android::dual_hil {

inline constexpr std::string_view kFieldRecordingHilAcceptedFailure =
    "IllegalStateException: HIL injected asynchronous field-recording start failure after "
    "acceptance";

struct FieldRecordingAsyncStartFailureEvidence {
  std::string failed_recording_id;
  std::string retry_recording_id;
  int down_initial_start_status = 0;
  int face_initial_start_status = 0;
  std::string face_initial_response_state;
  bool face_fault_armed_before_start = false;
  bool face_start_waiting_after_both_accepted = false;
  bool face_fault_consumed_after_failure = false;
  std::string face_failure_state;
  std::string face_failure_recording_id;
  std::string face_failure_error;
  int down_rollback_stop_status = 0;
  int face_failure_ack_status = 0;
  std::string down_rollback_state;
  std::string down_rollback_recording_id;
  std::string face_acknowledged_state;
  std::string face_acknowledged_recording_id;
  bool face_failure_acknowledged = false;
  bool down_rollback_bundle_visible = false;
  bool face_failed_bundle_absent = false;
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
  bool down_retry_bundle_visible = false;
  bool face_retry_bundle_visible = false;
  bool cleanup_passed = false;
  std::int64_t elapsed_ms = 0;
};

void ValidateFieldRecordingAsyncStartFailure(
    const FieldRecordingAsyncStartFailureEvidence &evidence);

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_FIELD_RECORDING_ASYNC_START_FAILURE_VALIDATION_H_
