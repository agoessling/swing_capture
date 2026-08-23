#include "android/dual_hil/autonomous_recovery_validation.h"

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace swing_capture::android::dual_hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

const Json &Pose(const Json &status) {
  if (status.value("state", "") == "error" || !status.contains("pose") ||
      !status.at("pose").is_object()) {
    throw std::runtime_error("Android node does not expose a healthy pose status");
  }
  return status.at("pose");
}

const Json &Autonomous(const Json &status) {
  const Json &pose = Pose(status);
  if (!pose.contains("autonomous_pair") || !pose.at("autonomous_pair").is_object()) {
    throw std::runtime_error("Android pose status has no autonomous-pair evidence");
  }
  return pose.at("autonomous_pair");
}

void RequireSafeExpectedId(std::string_view session_id) {
  if (session_id.empty() || session_id.size() > 128U) {
    throw std::invalid_argument("expected autonomous shared-session ID is invalid");
  }
  for (const char value : session_id) {
    const bool safe = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                      (value >= '0' && value <= '9') || value == '.' || value == '_' ||
                      value == '-';
    if (!safe) {
      throw std::invalid_argument("expected autonomous shared-session ID is invalid");
    }
  }
}

Json ParseStatus(std::string_view value) {
  try {
    return Json::parse(value);
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("Android status is not valid JSON: ") + failure.what());
  }
}

}  // namespace

void ValidateAutonomousLocalOnlyRecovery(const AutonomousLocalOnlyRecoveryInspection &inspection) {
  RequireSafeExpectedId(inspection.expected_shared_session_id);
  const Json status = ParseStatus(inspection.leader_status_json);
  const Json &autonomous = Autonomous(status);
  const std::string lifecycle = autonomous.value("state", "");
  if (!autonomous.value("recovered_from_checkpoint", false) ||
      autonomous.value("last_completed_shared_session_id", "") !=
          inspection.expected_shared_session_id ||
      autonomous.value("last_outcome", "") != "local_only" ||
      autonomous.value("replication_backlog_size", -1) != 1 ||
      autonomous.value("last_backlog_shared_session_id", "") !=
          inspection.expected_shared_session_id ||
      !autonomous.at("active_shared_session_id").is_null() ||
      (lifecycle != "degraded_monitoring" && lifecycle != "starting_station")) {
    throw std::runtime_error(
        "leader did not retain one local-only immutable record while its peer was unavailable");
  }
}

void ValidateAutonomousRecoveredStation(const AutonomousRecoveredStationInspection &inspection) {
  RequireSafeExpectedId(inspection.expected_shared_session_id);
  const Json leader = ParseStatus(inspection.leader_status_json);
  const Json shadow = ParseStatus(inspection.shadow_status_json);
  const Json &leader_pose = Pose(leader);
  const Json &shadow_pose = Pose(shadow);
  const Json &autonomous = Autonomous(leader);
  if (leader.value("state", "") != "armed" || !leader.value("armed", false) ||
      leader_pose.value("phase", "") != "monitoring" || leader_pose.value("mode", "") != "leader" ||
      shadow.value("state", "") != "armed" || !shadow.value("armed", false) ||
      shadow_pose.value("phase", "") != "monitoring" || shadow_pose.value("mode", "") != "shadow" ||
      autonomous.value("state", "") != "monitoring" ||
      autonomous.value("last_completed_shared_session_id", "") !=
          inspection.expected_shared_session_id ||
      autonomous.value("last_outcome", "") != "local_only" ||
      autonomous.value("replication_backlog_size", -1) != 0 ||
      autonomous.value("last_backlog_shared_session_id", "") !=
          inspection.expected_shared_session_id ||
      autonomous.value("last_backlog_outcome", "") != "replicated" ||
      !autonomous.at("active_shared_session_id").is_null()) {
    throw std::runtime_error(
        "two-phone station did not replicate its backlog and automatically return to monitoring");
  }
}

void ValidateAutonomousPeerUnavailable(std::string_view leader_status_json) {
  const Json status = ParseStatus(leader_status_json);
  const Json &autonomous = Autonomous(status);
  const std::string lifecycle = autonomous.value("state", "");
  if (autonomous.value("peer_available", true) ||
      (lifecycle != "degraded_monitoring" && lifecycle != "starting_station")) {
    throw std::runtime_error("leader did not detect the unavailable peer");
  }
}

}  // namespace swing_capture::android::dual_hil
