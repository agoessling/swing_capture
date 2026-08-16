#include "android/dual_hil/concurrent_hil_validation.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

#include "android/dual_coordination_hil/dual_coordination.h"

namespace swing_capture::android::dual_hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
namespace coordination = swing_capture::android::dual_coordination_hil;

[[noreturn]] void Invalid(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

std::int64_t DecimalString(const Json &value, std::string_view field) {
  if (!value.is_string()) {
    Invalid(std::string(field) + " must be a decimal string");
  }
  try {
    std::size_t consumed = 0;
    const std::string text = value.get<std::string>();
    const std::int64_t parsed = std::stoll(text, &consumed);
    if (consumed != text.size()) {
      Invalid(std::string(field) + " contains trailing characters");
    }
    return parsed;
  } catch (const std::exception &) {
    Invalid(std::string(field) + " is outside the signed 64-bit range");
  }
}

coordination::CaptureRole ParseRole(std::string_view role) {
  if (role == "down_the_line") {
    return coordination::CaptureRole::kDownTheLine;
  }
  if (role == "face_on") {
    return coordination::CaptureRole::kFaceOn;
  }
  Invalid("Android node returned an unknown role");
}

}  // namespace

NodeApiIdentity ValidateNodeDescriptor(std::string_view descriptor_json,
                                       coordination::CaptureRole expected_role,
                                       std::string_view expected_profile) {
  try {
    const Json descriptor = Json::parse(descriptor_json);
    NodeApiIdentity identity{
        .node_id = descriptor.value("node_id", ""),
        .role = ParseRole(descriptor.value("role", "")),
        .capture_profile = descriptor.value("capture_profile", ""),
    };
    if (descriptor.value("schema_version", 0) != 1 || identity.node_id.empty() ||
        identity.role != expected_role || identity.capture_profile != expected_profile ||
        descriptor.value("control_authentication", "") != "bearer") {
      Invalid("Android node descriptor does not match its fixed HIL identity/profile");
    }
    return identity;
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse Android node descriptor: ") +
                             failure.what());
  }
}

void ValidateArmedCaptureStatus(const ArmedStatusInspection &inspection) {
  try {
    const Json status = Json::parse(inspection.status_json);
    const std::string error = status.value("error", "not empty");
    if (status.value("schema_version", 0) != 2 || status.value("state", "") != "armed" ||
        !status.value("armed", false) ||
        status.value("shared_session_id", "") != inspection.shared_session_id || !error.empty()) {
      Invalid("Android node did not reach ARMED for the requested shared session");
    }
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse Android capture status: ") + failure.what());
  }
}

coordination::TriggerReport ValidateTriggerReport(std::string_view trigger_report_json,
                                                  const NodeApiIdentity &identity,
                                                  std::string_view shared_session_id,
                                                  std::string_view local_session_id) {
  try {
    const Json report = Json::parse(trigger_report_json);
    coordination::TriggerReport trigger{
        .role = ParseRole(report.value("role", "")),
        .node_id = report.value("node_id", ""),
        .shared_session_id = report.value("shared_session_id", ""),
        .local_session_id = report.value("local_session_id", ""),
        .trigger_timestamp_ns =
            DecimalString(report.at("trigger_elapsed_realtime_ns"), "trigger_elapsed_realtime_ns"),
        .trigger_uncertainty_ns = report.value("timestamp_uncertainty_ns", -1L),
        .source = report.value("source", ""),
    };
    if (report.value("schema_version", 0) != 1 || trigger.role != identity.role ||
        trigger.node_id != identity.node_id || trigger.shared_session_id != shared_session_id ||
        trigger.local_session_id != local_session_id || trigger.trigger_timestamp_ns <= 0 ||
        trigger.trigger_uncertainty_ns < 0 || trigger.source != "local_audio") {
      Invalid("Android trigger report identity or local-audio evidence is invalid");
    }
    return trigger;
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse Android trigger report: ") + failure.what());
  }
}

void ValidateCanonicalCoordinationReplay(std::string_view response_body,
                                         std::string_view canonical_json) {
  if (response_body.ends_with('\n')) {
    response_body.remove_suffix(1U);
  }
  if (response_body != canonical_json) {
    Invalid("Android node coordination replay differs from the canonical paired record");
  }
}

}  // namespace swing_capture::android::dual_hil
