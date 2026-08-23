#include "android/dual_hil/field_recording_async_start_failure_validation.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace swing_capture::android::dual_hil {
namespace {

void Require(bool condition, std::string_view diagnostic) {
  if (!condition) {
    throw std::runtime_error(std::string(diagnostic));
  }
}

}  // namespace

void ValidateFieldRecordingAsyncStartFailure(
    const FieldRecordingAsyncStartFailureEvidence &evidence) {
  Require(!evidence.failed_recording_id.empty(), "failed recording ID is missing");
  Require(!evidence.retry_recording_id.empty(), "retry recording ID is missing");
  Require(evidence.failed_recording_id != evidence.retry_recording_id,
          "retry reused the failed recording ID");
  Require(evidence.down_initial_start_status == 202 && evidence.face_initial_start_status == 202,
          "both initial field-recording starts were not accepted");
  Require(evidence.face_initial_response_state == "starting",
          "faulted node did not acknowledge a pending asynchronous start");
  Require(evidence.face_fault_armed_before_start,
          "post-acceptance start fault was not armed before the request");
  Require(evidence.face_start_waiting_after_both_accepted,
          "faulted start was not held until both HTTP 202 responses arrived");
  Require(evidence.face_fault_consumed_after_failure,
          "post-acceptance start fault was not consumed exactly once");
  Require(evidence.face_failure_state == "error", "faulted node did not surface a terminal error");
  Require(evidence.face_failure_recording_id == evidence.failed_recording_id,
          "faulted node error belongs to another recording");
  Require(evidence.face_failure_error == kFieldRecordingHilAcceptedFailure,
          "faulted node did not surface the stable injected diagnostic");
  Require(evidence.down_rollback_stop_status == 202 && evidence.face_failure_ack_status == 202,
          "coordinator did not stop or acknowledge both accepted nodes");
  Require(evidence.down_rollback_state == "ready" &&
              evidence.down_rollback_recording_id == evidence.failed_recording_id,
          "healthy node rollback did not publish the accepted recording");
  Require(evidence.face_acknowledged_state == "idle" &&
              evidence.face_acknowledged_recording_id.empty() && evidence.face_failure_acknowledged,
          "faulted node terminal error was not acknowledged to idle");
  Require(evidence.down_rollback_bundle_visible, "healthy node rollback bundle is not visible");
  Require(evidence.face_failed_bundle_absent,
          "faulted node advertised an incomplete recording bundle");
  Require(evidence.down_retry_start_status == 202 && evidence.face_retry_start_status == 202,
          "fresh-ID retry was not accepted by both nodes");
  Require(evidence.down_retry_recording_state == "recording" &&
              evidence.face_retry_recording_state == "recording",
          "fresh-ID retry did not reach recording on both nodes");
  Require(evidence.down_retry_recording_id == evidence.retry_recording_id &&
              evidence.face_retry_recording_id == evidence.retry_recording_id,
          "fresh-ID recording state is not coordinated");
  Require(evidence.down_retry_stop_status == 202 && evidence.face_retry_stop_status == 202,
          "fresh-ID retry did not accept both stops");
  Require(evidence.down_terminal_state == "ready" && evidence.face_terminal_state == "ready",
          "fresh-ID retry did not terminate ready on both nodes");
  Require(evidence.down_terminal_recording_id == evidence.retry_recording_id &&
              evidence.face_terminal_recording_id == evidence.retry_recording_id,
          "fresh-ID terminal state is not coordinated");
  Require(evidence.down_retry_bundle_visible && evidence.face_retry_bundle_visible,
          "fresh-ID retry did not publish both recording bundles");
  Require(evidence.cleanup_passed, "physical HIL cleanup did not pass");
  Require(evidence.elapsed_ms >= 0 && evidence.elapsed_ms < 15'000,
          "asynchronous start-failure HIL exceeded its 15-second deadline");
}

}  // namespace swing_capture::android::dual_hil
