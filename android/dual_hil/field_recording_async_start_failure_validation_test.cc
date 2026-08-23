#include "android/dual_hil/field_recording_async_start_failure_validation.h"

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace swing_capture::android::dual_hil {
namespace {

FieldRecordingAsyncStartFailureEvidence Nominal() {
  return {
      .failed_recording_id = "async-hil-first",
      .retry_recording_id = "async-hil-retry",
      .down_initial_start_status = 202,
      .face_initial_start_status = 202,
      .face_initial_response_state = "starting",
      .face_fault_armed_before_start = true,
      .face_start_waiting_after_both_accepted = true,
      .face_fault_consumed_after_failure = true,
      .face_failure_state = "error",
      .face_failure_recording_id = "async-hil-first",
      .face_failure_error = std::string(kFieldRecordingHilAcceptedFailure),
      .down_rollback_stop_status = 202,
      .face_failure_ack_status = 202,
      .down_rollback_state = "ready",
      .down_rollback_recording_id = "async-hil-first",
      .face_acknowledged_state = "idle",
      .face_acknowledged_recording_id = "",
      .face_failure_acknowledged = true,
      .down_rollback_bundle_visible = true,
      .face_failed_bundle_absent = true,
      .down_retry_start_status = 202,
      .face_retry_start_status = 202,
      .down_retry_recording_state = "recording",
      .down_retry_recording_id = "async-hil-retry",
      .face_retry_recording_state = "recording",
      .face_retry_recording_id = "async-hil-retry",
      .down_retry_stop_status = 202,
      .face_retry_stop_status = 202,
      .down_terminal_state = "ready",
      .down_terminal_recording_id = "async-hil-retry",
      .face_terminal_state = "ready",
      .face_terminal_recording_id = "async-hil-retry",
      .down_retry_bundle_visible = true,
      .face_retry_bundle_visible = true,
      .cleanup_passed = true,
      .elapsed_ms = 9'999,
  };
}

void ExpectRejected(std::function<void(FieldRecordingAsyncStartFailureEvidence &)> corrupt,
                    const std::string &name) {
  FieldRecordingAsyncStartFailureEvidence evidence = Nominal();
  corrupt(evidence);
  try {
    ValidateFieldRecordingAsyncStartFailure(evidence);
  } catch (const std::runtime_error &) {
    return;
  }
  throw std::runtime_error("invalid asynchronous start-failure evidence was accepted: " + name);
}

}  // namespace
}  // namespace swing_capture::android::dual_hil

int main() {
  using swing_capture::android::dual_hil::ExpectRejected;
  using swing_capture::android::dual_hil::FieldRecordingAsyncStartFailureEvidence;
  using swing_capture::android::dual_hil::Nominal;
  using swing_capture::android::dual_hil::ValidateFieldRecordingAsyncStartFailure;

  ValidateFieldRecordingAsyncStartFailure(Nominal());
  const std::vector<
      std::pair<std::string, std::function<void(FieldRecordingAsyncStartFailureEvidence &)>>>
      cases = {
          {"same ID", [](auto &value) { value.retry_recording_id = value.failed_recording_id; }},
          {"down start rejected", [](auto &value) { value.down_initial_start_status = 409; }},
          {"face start rejected", [](auto &value) { value.face_initial_start_status = 409; }},
          {"synchronous response",
           [](auto &value) { value.face_initial_response_state = "error"; }},
          {"fault not armed", [](auto &value) { value.face_fault_armed_before_start = false; }},
          {"not held after 202",
           [](auto &value) { value.face_start_waiting_after_both_accepted = false; }},
          {"fault not consumed",
           [](auto &value) { value.face_fault_consumed_after_failure = false; }},
          {"missing terminal error", [](auto &value) { value.face_failure_state = "starting"; }},
          {"wrong diagnostic",
           [](auto &value) { value.face_failure_error = "camera unavailable"; }},
          {"missing healthy rollback", [](auto &value) { value.down_rollback_stop_status = 0; }},
          {"missing failure ack", [](auto &value) { value.face_failure_ack_status = 0; }},
          {"failed ack remains error",
           [](auto &value) { value.face_acknowledged_state = "error"; }},
          {"failed bundle visible", [](auto &value) { value.face_failed_bundle_absent = false; }},
          {"retry rejected", [](auto &value) { value.face_retry_start_status = 409; }},
          {"retry divergent", [](auto &value) { value.face_retry_recording_id = "another"; }},
          {"retry stop", [](auto &value) { value.down_retry_stop_status = 500; }},
          {"retry publication", [](auto &value) { value.face_retry_bundle_visible = false; }},
          {"cleanup", [](auto &value) { value.cleanup_passed = false; }},
          {"deadline", [](auto &value) { value.elapsed_ms = 15'000; }},
      };
  for (const auto &[name, corrupt] : cases) {
    ExpectRejected(corrupt, name);
  }
  return 0;
}
