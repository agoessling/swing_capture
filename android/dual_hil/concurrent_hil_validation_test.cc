#include "android/dual_hil/concurrent_hil_validation.h"

#include <cassert>
#include <exception>
#include <nlohmann/json.hpp>
#include <string>

#include "android/dual_coordination_hil/dual_coordination.h"

namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
namespace coordination = swing_capture::android::dual_coordination_hil;
using swing_capture::android::dual_hil::ArmedStatusInspection;
using swing_capture::android::dual_hil::NodeApiIdentity;
using swing_capture::android::dual_hil::ValidateArmedCaptureStatus;
using swing_capture::android::dual_hil::ValidateCanonicalCoordinationReplay;
using swing_capture::android::dual_hil::ValidateNodeDescriptor;
using swing_capture::android::dual_hil::ValidateTriggerReport;

template <typename Action>
void ExpectFailure(Action action) {
  try {
    action();
  } catch (const std::exception &) {
    return;
  }
  assert(false);
}

NodeApiIdentity Identity() {
  return ValidateNodeDescriptor(Json({
                                         {"schema_version", 1},
                                         {"node_id", "node-dtl"},
                                         {"role", "down_the_line"},
                                         {"capture_profile", "720p240"},
                                         {"service_urls", Json::array()},
                                         {"control_authentication", "bearer"},
                                     })
                                    .dump(),
                                coordination::CaptureRole::kDownTheLine, "720p240");
}

void ExactDescriptorAndArmedStatusPass() {
  const NodeApiIdentity identity = Identity();
  assert(identity.node_id == "node-dtl");
  ValidateArmedCaptureStatus(ArmedStatusInspection{
      .status_json = Json({
                              {"schema_version", 2},
                              {"state", "armed"},
                              {"armed", true},
                              {"shared_session_id", "shared-1"},
                              {"error", ""},
                          })
                         .dump(),
      .shared_session_id = "shared-1",
  });
}

void IdentityAndSessionMismatchFail() {
  Json descriptor = {
      {"schema_version", 1},
      {"node_id", "node-dtl"},
      {"role", "face_on"},
      {"capture_profile", "720p240"},
      {"control_authentication", "bearer"},
  };
  ExpectFailure([&] {
    static_cast<void>(ValidateNodeDescriptor(descriptor.dump(),
                                             coordination::CaptureRole::kDownTheLine, "720p240"));
  });
  const Json status = {
      {"schema_version", 2},          {"state", "armed"}, {"armed", true},
      {"shared_session_id", "other"}, {"error", ""},
  };
  ExpectFailure([&] {
    ValidateArmedCaptureStatus(
        ArmedStatusInspection{.status_json = status.dump(), .shared_session_id = "shared-1"});
  });
}

void ExactTriggerReportPassesAndCorruptionFails() {
  const NodeApiIdentity identity = Identity();
  Json report = {
      {"schema_version", 1},
      {"role", "down_the_line"},
      {"node_id", "node-dtl"},
      {"shared_session_id", "shared-1"},
      {"local_session_id", "local-dtl"},
      {"trigger_elapsed_realtime_ns", "123456789"},
      {"timestamp_uncertainty_ns", 400000},
      {"source", "local_audio"},
  };
  const auto trigger = ValidateTriggerReport(report.dump(), identity, "shared-1", "local-dtl");
  assert(trigger.trigger_timestamp_ns == 123456789L);
  report["local_session_id"] = "foreign";
  ExpectFailure([&] {
    static_cast<void>(ValidateTriggerReport(report.dump(), identity, "shared-1", "local-dtl"));
  });
  report["local_session_id"] = "local-dtl";
  report["source"] = "manual";
  ExpectFailure([&] {
    static_cast<void>(ValidateTriggerReport(report.dump(), identity, "shared-1", "local-dtl"));
  });
}

void CanonicalReplayIsByteExactExceptForFramingNewline() {
  constexpr std::string_view kCanonical = R"({"schema_version":1,"status":"paired"})";
  ValidateCanonicalCoordinationReplay(std::string(kCanonical) + "\n", kCanonical);
  ExpectFailure([&] {
    ValidateCanonicalCoordinationReplay(R"({"status":"paired","schema_version":1})", kCanonical);
  });
  ExpectFailure(
      [&] { ValidateCanonicalCoordinationReplay(std::string(kCanonical) + "\n\n", kCanonical); });
}

}  // namespace

int main() {
  ExactDescriptorAndArmedStatusPass();
  IdentityAndSessionMismatchFail();
  ExactTriggerReportPassesAndCorruptionFails();
  CanonicalReplayIsByteExactExceptForFramingNewline();
  return 0;
}
