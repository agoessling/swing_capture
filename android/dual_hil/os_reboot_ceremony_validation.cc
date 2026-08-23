#include "android/dual_hil/os_reboot_ceremony_validation.h"

#include <charconv>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>  // NOLINT(misc-include-cleaner)
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace swing_capture::android::dual_hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

constexpr std::int64_t kMaximumPeerClockAgeNs = 10'000'000'000LL;
constexpr std::int64_t kMaximumPeerClockUncertaintyNs = 25'000'000LL;

// The two strings have intentionally distinct document and diagnostic-label roles.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Json ParseObject(std::string_view document, std::string_view name) {
  Json parsed;
  try {
    parsed = Json::parse(document);
  } catch (const Json::exception &) {
    throw std::runtime_error(std::string(name) + " is not valid JSON");
  }
  if (!parsed.is_object()) {
    throw std::runtime_error(std::string(name) + " must be an object");
  }
  return parsed;
}

std::int64_t NonnegativeInteger(const Json &value, std::string_view name) {
  if (!value.is_number_integer()) {
    throw std::runtime_error(std::string(name) + " must be a nonnegative integer");
  }
  if (value.is_number_unsigned()) {
    const std::uint64_t parsed = value.get<std::uint64_t>();
    if (parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      throw std::runtime_error(std::string(name) + " exceeds the signed 64-bit range");
    }
    return static_cast<std::int64_t>(parsed);
  }
  const std::int64_t parsed = value.get<std::int64_t>();
  if (parsed < 0) {
    throw std::runtime_error(std::string(name) + " must be a nonnegative integer");
  }
  return parsed;
}

void RequireSchemaVersion(const Json &document, std::int64_t expected, std::string_view context) {
  const auto version = document.find("schema_version");
  if (version == document.end() ||
      NonnegativeInteger(*version, std::string(context) + " schema_version") != expected) {
    throw std::runtime_error(std::string(context) + " schema version is invalid");
  }
}

std::int64_t DecimalString(const Json &value, std::string_view name) {
  if (!value.is_string()) {
    throw std::runtime_error(std::string(name) + " must be a decimal string");
  }
  const auto &text = value.get_ref<const std::string &>();
  std::int64_t parsed = 0;
  // std::from_chars expresses its range as two pointers.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto *text_end = text.data() + text.size();
  const auto [end, error] = std::from_chars(text.data(), text_end, parsed);
  if (error != std::errc() || end != text_end || parsed < 0) {
    throw std::runtime_error(std::string(name) + " is not a nonnegative decimal string");
  }
  return parsed;
}

void RequireRebootContract(const Json &recovery, bool require_ready, std::string_view context) {
  if (!recovery.is_object()) {
    throw std::runtime_error(std::string(context) + " reboot policy contract is invalid");
  }
  RequireSchemaVersion(recovery, 1, std::string(context) + " reboot policy");
  if (recovery.value("mode", "") != "operator_launch_after_first_unlock" ||
      recovery.value("direct_boot_aware", true) ||
      !recovery.value("boot_receiver_registered", false) ||
      recovery.value("automatic_capture_before_first_unlock", true) ||
      !recovery.value("operator_foreground_launch_required_after_os_reboot", false) ||
      recovery.value("process_restart_policy", "") != "android_start_sticky_best_effort") {
    throw std::runtime_error(std::string(context) + " reboot policy contract is invalid");
  }
  if (require_ready &&
      (!recovery.value("user_unlocked", false) || !recovery.value("service_running", false) ||
       !recovery.value("ready_this_boot", false) || !recovery.at("issues").is_array() ||
       !recovery.at("issues").empty())) {
    throw std::runtime_error(std::string(context) + " is not ready after unlock and launch");
  }
}

Json DurableSetup(const Json &setup) {
  const Json &node = setup.at("node");
  Json durable = {
      {"node_id", node.at("node_id")},
      {"control_credential_generation", node.at("control_credential_generation")},
      {"configuration", setup.at("configuration")},
      {"pairing", nullptr},
  };
  if (!setup.at("pairing").is_null()) {
    const Json &pairing = setup.at("pairing");
    durable["pairing"] = {
        {"state", pairing.at("state")},
        {"peer_node_id", pairing.at("peer_node_id")},
        {"expected_role", pairing.at("expected_role")},
        {"label", pairing.at("label")},
        {"origin", pairing.at("origin")},
        {"credential_generation", pairing.at("credential_generation")},
    };
  }
  return durable;
}

void RequireReadyBaseline(const Json &status, std::string_view role) {
  RequireSchemaVersion(status, 2, std::string(role) + " baseline status");
  if (status.value("state", "") != "ready" || status.value("armed", true)) {
    throw std::runtime_error(std::string(role) + " was not safely READY/unarmed before reboot");
  }
  RequireRebootContract(status.at("reboot_recovery"), true, std::string(role) + " baseline");
}

std::int64_t BootObservationCount(const Json &document, std::string_view context,
                                  bool require_observed) {
  const Json &observation = document.at("reboot_recovery").at("boot_observation");
  if (!observation.is_object() || !observation.value("available", false)) {
    throw std::runtime_error(std::string(context) + " boot marker is unavailable");
  }
  const std::int64_t count = DecimalString(observation.at("observation_count"),
                                           std::string(context) + " observation count");
  if (require_observed && (!observation.value("observed", false) || count <= 0 ||
                           (observation.value("last_event", "") != "locked_boot_completed" &&
                            observation.value("last_event", "") != "boot_completed" &&
                            observation.value("last_event", "") != "user_unlocked") ||
                           !observation.at("last_observed_epoch_ms").is_number_integer() ||
                           observation.at("last_observed_epoch_ms").get<std::int64_t>() <= 0 ||
                           DecimalString(observation.at("last_observed_elapsed_realtime_ns"),
                                         std::string(context) + " boot elapsed time") <= 0)) {
    throw std::runtime_error(std::string(context) + " lacks a valid post-reboot observation");
  }
  return count;
}

void RequireArmedMonitoring(const Json &status, std::string_view role) {
  RequireSchemaVersion(status, 2, std::string(role) + " armed status");
  if (status.value("state", "") != "armed" || !status.value("armed", false)) {
    throw std::runtime_error(std::string(role) + " did not rearm");
  }
  const Json &pose = status.at("pose");
  const std::string expected_mode = role == "face_on" ? "leader" : "shadow";
  if (pose.value("mode", "") != expected_mode || pose.value("phase", "") != "monitoring" ||
      !pose.at("standby_audio").value("ready", false) ||
      pose.at("metrics").value("successful_inferences", 0LL) < 1) {
    throw std::runtime_error(std::string(role) + " did not restore live monitoring");
  }
}

void RequireLeaderPeerClockRecovery(const Json &status, std::string_view expected_peer_node_id) {
  if (expected_peer_node_id.empty()) {
    throw std::runtime_error("recovered leader has no expected shadow node ID");
  }
  const Json &pose = status.at("pose");
  const Json &autonomous = pose.at("autonomous_pair");
  const Json &clock = pose.at("peer_clock");
  if (!autonomous.is_object() || !clock.is_object()) {
    throw std::runtime_error("face_on leader lacks peer recovery evidence");
  }
  const auto peer_node_id = clock.find("peer_node_id");
  if (peer_node_id == clock.end() || !peer_node_id->is_string()) {
    throw std::runtime_error("face_on peer clock peer_node_id must be a string");
  }
  const std::int64_t age = DecimalString(clock.at("age_ns"), "face_on peer clock age_ns");
  const std::int64_t uncertainty =
      DecimalString(clock.at("uncertainty_ns"), "face_on peer clock uncertainty_ns");
  const std::int64_t sample_count =
      NonnegativeInteger(clock.at("sample_count"), "face_on peer clock sample_count");
  if (!pose.value("peer_configured", false) || autonomous.value("state", "") != "monitoring" ||
      !autonomous.value("peer_available", false) ||
      peer_node_id->get_ref<const std::string &>() != expected_peer_node_id || sample_count < 1 ||
      age > kMaximumPeerClockAgeNs || uncertainty > kMaximumPeerClockUncertaintyNs) {
    throw std::runtime_error("face_on leader did not recover a usable live shadow clock");
  }
}

void RequireCleanup(const Json &status, const OsRebootNodeInspection &node) {
  RequireSchemaVersion(status, 2, node.role + " cleanup status");
  if (!node.disarm_acknowledged || status.value("state", "") != "ready" ||
      status.value("armed", true)) {
    throw std::runtime_error(node.role + " did not return to READY/unarmed during cleanup");
  }
}

OsRebootNodeEvidence ValidateNode(const OsRebootNodeInspection &node) {
  if (node.role != "face_on" && node.role != "down_the_line") {
    throw std::runtime_error("reboot ceremony node has an invalid role");
  }
  if (!node.exact_apk_verified || !node.reboot_command_accepted ||
      !node.user_unlocked_before_launch || !node.capture_service_absent_before_launch ||
      !node.node_api_unavailable_before_launch || !node.foreground_activity_observed ||
      node.adb_foreground_launch_sent) {
    throw std::runtime_error(node.role + " lacks the required human reboot ceremony evidence");
  }
  if (node.before_boot_id.empty() || node.after_boot_id.empty() ||
      node.before_boot_id == node.after_boot_id) {
    throw std::runtime_error(node.role + " does not prove a changed Linux boot ID");
  }

  const Json before_setup = ParseObject(node.before_setup_json, node.role + " before setup");
  const Json before_status = ParseObject(node.before_status_json, node.role + " before status");
  const Json after_setup = ParseObject(node.after_setup_json, node.role + " after setup");
  const Json after_launch =
      ParseObject(node.after_launch_status_json, node.role + " after launch status");
  const Json armed = ParseObject(node.armed_status_json, node.role + " armed status");
  const Json cleanup = ParseObject(node.cleanup_status_json, node.role + " cleanup status");
  const Json cleanup_setup = ParseObject(node.cleanup_setup_json, node.role + " cleanup setup");

  RequireSchemaVersion(before_setup, 1, node.role + " before setup");
  RequireSchemaVersion(after_setup, 1, node.role + " after setup");
  RequireSchemaVersion(cleanup_setup, 1, node.role + " cleanup setup");
  RequireReadyBaseline(before_status, node.role);
  RequireSchemaVersion(after_launch, 2, node.role + " post-launch status");
  RequireRebootContract(after_setup.at("reboot_recovery"), true, node.role + " post-launch setup");
  RequireRebootContract(after_launch.at("reboot_recovery"), true,
                        node.role + " post-launch status");
  if (DurableSetup(before_setup) != DurableSetup(after_setup)) {
    throw std::runtime_error(node.role + " durable configuration changed across reboot");
  }
  if (DurableSetup(before_setup) != DurableSetup(cleanup_setup)) {
    throw std::runtime_error(node.role + " durable configuration changed during rearm cleanup");
  }
  const Json &configuration = after_setup.at("configuration");
  const Json &pose = configuration.at("pose");
  if (configuration.value("role", "") != node.role ||
      configuration.value("capture_profile", "") != "720p240" ||
      pose.value("mode", "") != (node.role == "face_on" ? "leader" : "shadow")) {
    throw std::runtime_error(node.role + " durable station assignment is not qualified");
  }
  if (node.role == "face_on") {
    const Json &pairing = after_setup.at("pairing");
    if (!pose.at("peer").is_object() || !pairing.is_object() ||
        pairing.value("state", "") != "active" ||
        pairing.value("expected_role", "") != "down_the_line" ||
        pairing.value("credential_status", "") != "verified") {
      throw std::runtime_error("face_on durable peer binding did not recover");
    }
  } else if (!pose.at("peer").is_null() || !after_setup.at("pairing").is_null()) {
    throw std::runtime_error("down_the_line shadow unexpectedly owns a peer binding");
  }

  const std::int64_t before_count =
      BootObservationCount(before_status, node.role + " baseline", false);
  const std::int64_t after_count =
      BootObservationCount(after_launch, node.role + " post-launch", true);
  if (after_count <= before_count) {
    throw std::runtime_error(node.role + " boot observation count did not advance");
  }
  RequireArmedMonitoring(armed, node.role);
  RequireCleanup(cleanup, node);
  const Json &node_id = after_setup.at("node").at("node_id");
  if (!node_id.is_string() || node_id.get_ref<const std::string &>().empty()) {
    throw std::runtime_error(node.role + " node ID is missing or malformed");
  }
  return {.role = node.role,
          .node_id = node_id.get_ref<const std::string &>(),
          .device_model = after_setup.at("node").at("device_model").get<std::string>(),
          .boot_observation_count_before = before_count,
          .boot_observation_count_after = after_count};
}

}  // namespace

OsRebootCeremonyEvidence ValidateOsRebootCeremony(const OsRebootCeremonyInspection &inspection) {
  try {
    OsRebootCeremonyEvidence evidence{
        .face_on = ValidateNode(inspection.face_on),
        .down_the_line = ValidateNode(inspection.down_the_line),
    };
    if (evidence.face_on.role != "face_on" || evidence.down_the_line.role != "down_the_line" ||
        evidence.face_on.node_id == evidence.down_the_line.node_id) {
      throw std::runtime_error("reboot ceremony does not contain two distinct complementary nodes");
    }
    const Json leader_setup = Json::parse(inspection.face_on.after_setup_json);
    if (leader_setup.at("pairing").value("peer_node_id", "") != evidence.down_the_line.node_id) {
      throw std::runtime_error("recovered leader binding identifies a different shadow node");
    }
    const Json leader_armed = Json::parse(inspection.face_on.armed_status_json);
    RequireLeaderPeerClockRecovery(leader_armed, evidence.down_the_line.node_id);
    return evidence;
  } catch (const Json::exception &failure) {
    throw std::runtime_error(std::string("reboot ceremony evidence has a malformed JSON schema: ") +
                             failure.what());
  }
}

}  // namespace swing_capture::android::dual_hil
