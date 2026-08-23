#include "android/dual_hil/os_reboot_ceremony_validation.h"

#include <cassert>
#include <functional>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using swing_capture::android::dual_hil::OsRebootCeremonyInspection;
using swing_capture::android::dual_hil::OsRebootNodeInspection;
using swing_capture::android::dual_hil::ValidateOsRebootCeremony;
using Json = nlohmann::json;

Json Recovery(long long count) {
  return {{"schema_version", 1},
          {"mode", "operator_launch_after_first_unlock"},
          {"direct_boot_aware", false},
          {"boot_receiver_registered", true},
          {"automatic_capture_before_first_unlock", false},
          {"operator_foreground_launch_required_after_os_reboot", true},
          {"process_restart_policy", "android_start_sticky_best_effort"},
          {"boot_observation",
           {{"available", true},
            {"observation_count", std::to_string(count)},
            {"observed", true},
            {"last_event", "user_unlocked"},
            {"last_observed_epoch_ms", 1000},
            {"last_observed_elapsed_realtime_ns", "100"}}},
          {"user_unlocked", true},
          {"service_running", true},
          {"ready_this_boot", true},
          {"issues", Json::array()}};
}

std::string Setup(std::string_view role, std::string_view node_id, long long verified_at,
                  bool after_reboot) {
  const bool leader = role == "face_on";
  Json pairing = nullptr;
  Json peer = nullptr;
  if (leader) {
    pairing = {{"state", "active"},
               {"peer_node_id", "dtl-id"},
               {"expected_role", "down_the_line"},
               {"label", "DTL"},
               {"origin", "http://10.0.0.2:8088"},
               {"credential_generation", 2},
               {"credential_status", "verified"},
               {"verified_at_epoch_ms", verified_at},
               {"revoked_at_epoch_ms", 0}};
    peer = {{"origin", "http://10.0.0.2:8088"}};
  }
  return Json{
      {"schema_version", 1},
      {"revision", 7},
      {"node",
       {{"node_id", node_id},
        {"control_credential_generation", 3},
        {"service_urls", Json::array({"http://10.0.0.1:8088"})},
        {"device_model", leader ? "Pixel 6" : "Pixel 5a"}}},
      {"configuration",
       {{"role", role},
        {"capture_profile", "720p240"},
        {"pose",
         {{"mode", leader ? "leader" : "shadow"},
          {"inference_delegate", "gpu_preferred"},
          {"debug_evidence_enabled", true},
          {"hitting_region", {{"left", 0.0}, {"top", 0.0}, {"right", 1.0}, {"bottom", 1.0}}},
          {"peer", peer}}}}},
      {"pairing", pairing},
      {"reboot_recovery", Recovery(after_reboot ? 8 : 5)}}
      .dump();
}

std::string Status(std::string_view role, bool armed, long long count) {
  const bool leader = role == "face_on";
  return Json{
      {"schema_version", 2},
      {"state", armed ? "armed" : "ready"},
      {"armed", armed},
      {"reboot_recovery", Recovery(count)},
      {"pose",
       {{"mode", leader ? "leader" : "shadow"},
        {"phase", armed ? "monitoring" : "stopped"},
        {"peer_configured", leader},
        {"metrics", {{"successful_inferences", 2}}},
        {"standby_audio", {{"ready", true}}},
        {"autonomous_pair",
         {{"state", leader && armed ? "monitoring" : "stopped"}, {"peer_available", leader}}},
        {"peer_clock", leader ? Json{{"peer_node_id", "dtl-id"},
                                     {"age_ns", "10000000000"},
                                     {"uncertainty_ns", "25000000"},
                                     {"sample_count", 1}}
                              : Json(nullptr)}}}}
      .dump();
}

OsRebootNodeInspection Node(std::string role, std::string node_id) {
  return {
      .role = role,
      .before_boot_id = "11111111-1111-1111-1111-111111111111",
      .after_boot_id = "22222222-2222-2222-2222-222222222222",
      .before_setup_json = Setup(role, node_id, 100, false),
      .before_status_json = Status(role, false, 5),
      .after_setup_json = Setup(role, node_id, 200, true),
      .after_launch_status_json = Status(role, false, 8),
      .armed_status_json = Status(role, true, 8),
      .cleanup_status_json = Status(role, false, 8),
      .cleanup_setup_json = Setup(role, node_id, 300, true),
      .exact_apk_verified = true,
      .reboot_command_accepted = true,
      .user_unlocked_before_launch = true,
      .capture_service_absent_before_launch = true,
      .node_api_unavailable_before_launch = true,
      .foreground_activity_observed = true,
      .adb_foreground_launch_sent = false,
      .disarm_acknowledged = true,
  };
}

OsRebootCeremonyInspection Passing() {
  return {.face_on = Node("face_on", "face-id"), .down_the_line = Node("down_the_line", "dtl-id")};
}

void Rejects(const std::function<void(OsRebootCeremonyInspection &)> &mutate) {
  OsRebootCeremonyInspection inspection = Passing();
  mutate(inspection);
  try {
    static_cast<void>(ValidateOsRebootCeremony(inspection));
  } catch (const std::runtime_error &) {
    return;
  }
  throw std::runtime_error("invalid reboot ceremony fixture was accepted");
}

void MutateJson(std::string *document, const std::function<void(Json &)> &mutate) {
  Json parsed = Json::parse(*document);
  mutate(parsed);
  *document = parsed.dump();
}

void ReplaceAll(std::string *document, std::string_view before, std::string_view after) {
  std::size_t position = 0;
  while ((position = document->find(before, position)) != std::string::npos) {
    document->replace(position, before.size(), after);
    position += after.size();
  }
}

}  // namespace

int main() {
  const auto passed = ValidateOsRebootCeremony(Passing());
  assert(passed.face_on.node_id == "face-id");
  assert(passed.down_the_line.boot_observation_count_after == 8);

  Rejects([](auto &fixture) { fixture.face_on.after_boot_id = fixture.face_on.before_boot_id; });
  Rejects([](auto &fixture) {
    fixture.face_on.after_launch_status_json = Status("face_on", false, 5);
  });
  Rejects([](auto &fixture) { fixture.face_on.capture_service_absent_before_launch = false; });
  Rejects([](auto &fixture) { fixture.face_on.node_api_unavailable_before_launch = false; });
  Rejects([](auto &fixture) { fixture.face_on.foreground_activity_observed = false; });
  Rejects([](auto &fixture) { fixture.face_on.adb_foreground_launch_sent = true; });
  Rejects([](auto &fixture) { fixture.down_the_line.exact_apk_verified = false; });
  Rejects([](auto &fixture) {
    fixture.face_on.after_setup_json = Setup("face_on", "other", 200, true);
  });
  Rejects([](auto &fixture) {
    ReplaceAll(&fixture.face_on.before_setup_json, "dtl-id", "unexpected-peer-id");
    ReplaceAll(&fixture.face_on.after_setup_json, "dtl-id", "unexpected-peer-id");
    ReplaceAll(&fixture.face_on.cleanup_setup_json, "dtl-id", "unexpected-peer-id");
  });
  Rejects([](auto &fixture) { fixture.face_on.armed_status_json = Status("face_on", false, 8); });
  Rejects([](auto &fixture) {
    MutateJson(&fixture.face_on.armed_status_json,
               [](Json &status) { status["pose"]["autonomous_pair"]["peer_available"] = false; });
  });
  Rejects([](auto &fixture) {
    MutateJson(&fixture.face_on.armed_status_json,
               [](Json &status) { status["pose"]["peer_clock"]["sample_count"] = 0; });
  });
  Rejects([](auto &fixture) {
    MutateJson(&fixture.face_on.armed_status_json,
               [](Json &status) { status["pose"]["peer_clock"]["age_ns"] = "10000000001"; });
  });
  Rejects([](auto &fixture) {
    MutateJson(&fixture.face_on.armed_status_json,
               [](Json &status) { status["pose"]["peer_clock"]["uncertainty_ns"] = "25000001"; });
  });
  Rejects([](auto &fixture) {
    MutateJson(&fixture.face_on.armed_status_json, [](Json &status) {
      status["pose"]["peer_clock"]["peer_node_id"] = "unexpected-peer-id";
    });
  });
  Rejects([](auto &fixture) {
    MutateJson(&fixture.face_on.armed_status_json, [](Json &status) {
      status["pose"]["autonomous_pair"]["state"] = "degraded_monitoring";
    });
  });
  Rejects([](auto &fixture) {
    MutateJson(&fixture.face_on.armed_status_json,
               [](Json &status) { status["pose"].erase("peer_clock"); });
  });
  Rejects([](auto &fixture) {
    MutateJson(&fixture.face_on.armed_status_json,
               [](Json &status) { status["pose"]["peer_clock"]["age_ns"] = 1000; });
  });
  Rejects([](auto &fixture) {
    MutateJson(&fixture.face_on.armed_status_json,
               [](Json &status) { status["pose"]["peer_clock"]["uncertainty_ns"] = "25ms"; });
  });
  Rejects([](auto &fixture) {
    MutateJson(&fixture.face_on.armed_status_json,
               [](Json &status) { status["pose"]["peer_clock"]["sample_count"] = "1"; });
  });
  Rejects([](auto &fixture) {
    MutateJson(&fixture.face_on.armed_status_json,
               [](Json &status) { status["schema_version"] = 2.0; });
  });
  Rejects([](auto &fixture) {
    MutateJson(&fixture.down_the_line.armed_status_json,
               [](Json &status) { status["pose"]["standby_audio"]["ready"] = false; });
  });
  Rejects([](auto &fixture) {
    MutateJson(&fixture.down_the_line.armed_status_json,
               [](Json &status) { status["pose"]["metrics"]["successful_inferences"] = 0; });
  });
  Rejects([](auto &fixture) { fixture.down_the_line.disarm_acknowledged = false; });
  Rejects([](auto &fixture) {
    fixture.down_the_line.cleanup_status_json = Status("down_the_line", true, 8);
  });
  Rejects([](auto &fixture) {
    fixture.down_the_line.cleanup_setup_json = Setup("down_the_line", "replacement", 300, true);
  });
}
