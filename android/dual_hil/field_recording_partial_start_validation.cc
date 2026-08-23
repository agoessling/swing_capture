#include "android/dual_hil/field_recording_partial_start_validation.h"

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

bool TerminalBeforeStart(std::string_view state) { return state == "idle" || state == "ready"; }

}  // namespace

void ValidateFieldRecordingPartialStart(const FieldRecordingPartialStartEvidence &evidence) {
  Require(!evidence.rejected_recording_id.empty(), "rejected recording ID is missing");
  Require(!evidence.retry_recording_id.empty(), "retry recording ID is missing");
  Require(evidence.rejected_recording_id != evidence.retry_recording_id,
          "retry reused the rejected recording ID");
  Require(evidence.down_initial_start_status == 202,
          "down-the-line node did not accept the initial start");
  Require(evidence.face_initial_start_status == 409,
          "face-on node did not synchronously reject the initial start");
  Require(evidence.face_initial_error == kFieldRecordingHilRejection,
          "face-on rejection diagnostic does not identify the HIL fault");
  Require(evidence.face_fault_armed_before_start,
          "face-on HIL fault was not armed before the rejected start");
  Require(evidence.face_fault_consumed_after_rejection,
          "face-on HIL fault was not consumed by the rejected start");
  Require(evidence.down_rollback_stop_status == 202,
          "coordinator did not stop the initially started down-the-line node");
  Require(evidence.down_rollback_state == "ready",
          "down-the-line rollback did not publish a terminal recording");
  Require(evidence.down_rollback_recording_id == evidence.rejected_recording_id,
          "down-the-line rollback belongs to another recording");
  Require(TerminalBeforeStart(evidence.face_after_rejection_state),
          "rejected face-on node is not terminal");
  Require(evidence.face_after_rejection_recording_id != evidence.rejected_recording_id,
          "rejected face-on node allocated the rejected recording ID");
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
  Require(evidence.cleanup_passed, "physical HIL cleanup did not pass");
  Require(evidence.elapsed_ms >= 0 && evidence.elapsed_ms < 15'000,
          "partial-start HIL exceeded its 15-second deadline");
}

}  // namespace swing_capture::android::dual_hil
