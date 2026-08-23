#include "android/dual_hil/field_recording_partial_start_validation.h"

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace swing_capture::android::dual_hil {
namespace {

FieldRecordingPartialStartEvidence Nominal() {
  return {
      .rejected_recording_id = "partial-hil-first",
      .retry_recording_id = "partial-hil-retry",
      .down_initial_start_status = 202,
      .face_initial_start_status = 409,
      .face_initial_error = std::string(kFieldRecordingHilRejection),
      .face_fault_armed_before_start = true,
      .face_fault_consumed_after_rejection = true,
      .down_rollback_stop_status = 202,
      .down_rollback_state = "ready",
      .down_rollback_recording_id = "partial-hil-first",
      .face_after_rejection_state = "idle",
      .face_after_rejection_recording_id = "",
      .down_retry_start_status = 202,
      .face_retry_start_status = 202,
      .down_retry_recording_state = "recording",
      .down_retry_recording_id = "partial-hil-retry",
      .face_retry_recording_state = "recording",
      .face_retry_recording_id = "partial-hil-retry",
      .down_retry_stop_status = 202,
      .face_retry_stop_status = 202,
      .down_terminal_state = "ready",
      .down_terminal_recording_id = "partial-hil-retry",
      .face_terminal_state = "ready",
      .face_terminal_recording_id = "partial-hil-retry",
      .cleanup_passed = true,
      .elapsed_ms = 9'999,
  };
}

void ExpectRejected(std::function<void(FieldRecordingPartialStartEvidence &)> corrupt,
                    const std::string &name) {
  FieldRecordingPartialStartEvidence evidence = Nominal();
  corrupt(evidence);
  try {
    ValidateFieldRecordingPartialStart(evidence);
  } catch (const std::runtime_error &) {
    return;
  }
  throw std::runtime_error("invalid partial-start evidence was accepted: " + name);
}

}  // namespace
}  // namespace swing_capture::android::dual_hil

int main() {
  using swing_capture::android::dual_hil::ExpectRejected;
  using swing_capture::android::dual_hil::FieldRecordingPartialStartEvidence;
  using swing_capture::android::dual_hil::Nominal;
  using swing_capture::android::dual_hil::ValidateFieldRecordingPartialStart;

  ValidateFieldRecordingPartialStart(Nominal());
  const std::vector<
      std::pair<std::string, std::function<void(FieldRecordingPartialStartEvidence &)>>>
      cases = {
          {"same ID", [](auto &value) { value.retry_recording_id = value.rejected_recording_id; }},
          {"first start", [](auto &value) { value.down_initial_start_status = 409; }},
          {"missing rejection", [](auto &value) { value.face_initial_start_status = 202; }},
          {"wrong rejection", [](auto &value) { value.face_initial_error = "camera unavailable"; }},
          {"fault not armed", [](auto &value) { value.face_fault_armed_before_start = false; }},
          {"fault not consumed",
           [](auto &value) { value.face_fault_consumed_after_rejection = false; }},
          {"missing rollback", [](auto &value) { value.down_rollback_stop_status = 0; }},
          {"rollback active", [](auto &value) { value.down_rollback_state = "stopping"; }},
          {"rejected allocation",
           [](auto &value) {
             value.face_after_rejection_recording_id = value.rejected_recording_id;
           }},
          {"retry rejected", [](auto &value) { value.face_retry_start_status = 409; }},
          {"retry divergent", [](auto &value) { value.face_retry_recording_id = "another"; }},
          {"retry stop", [](auto &value) { value.down_retry_stop_status = 500; }},
          {"terminal error", [](auto &value) { value.face_terminal_state = "error"; }},
          {"cleanup", [](auto &value) { value.cleanup_passed = false; }},
          {"deadline", [](auto &value) { value.elapsed_ms = 15'000; }},
      };
  for (const auto &[name, corrupt] : cases) {
    ExpectRejected(corrupt, name);
  }
  return 0;
}
