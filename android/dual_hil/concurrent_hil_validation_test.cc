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
using swing_capture::android::dual_hil::DurablePairingState;
using swing_capture::android::dual_hil::HasPairedAutomaticImpactEvidence;
using swing_capture::android::dual_hil::InspectDiscoveryPairingFixture;
using swing_capture::android::dual_hil::InspectPairNetworkHealthForArm;
using swing_capture::android::dual_hil::LanEndpointInspection;
using swing_capture::android::dual_hil::MappedPeerImpactStatusInspection;
using swing_capture::android::dual_hil::NodeApiIdentity;
using swing_capture::android::dual_hil::PairedPoseClockStatusInspection;
using swing_capture::android::dual_hil::PairedPoseSessionStateInspection;
using swing_capture::android::dual_hil::PairNetworkHealthArmDecision;
using swing_capture::android::dual_hil::PairNetworkHealthArmRequestBody;
using swing_capture::android::dual_hil::PairNetworkHealthStatusInspection;
using swing_capture::android::dual_hil::PoseConfiguredDescriptorInspection;
using swing_capture::android::dual_hil::ValidateArmedCaptureStatus;
using swing_capture::android::dual_hil::ValidateCanonicalCoordinationReplay;
using swing_capture::android::dual_hil::ValidateLanEndpoint;
using swing_capture::android::dual_hil::ValidateMappedPeerImpactStatus;
using swing_capture::android::dual_hil::ValidateNodeDescriptor;
using swing_capture::android::dual_hil::ValidatePairedPoseClockStatus;
using swing_capture::android::dual_hil::ValidatePairedPoseHilReport;
using swing_capture::android::dual_hil::ValidatePairedPoseSessionState;
using swing_capture::android::dual_hil::ValidatePoseConfiguredNodeDescriptor;
using swing_capture::android::dual_hil::ValidateShortPoseLatency;
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

Json PairNetworkDirectionFixture();
Json PairNetworkStatusFixture(std::string_view state);

Json PairedPoseReportFixture() {
  constexpr std::string_view kSharedSessionId = "pose-hil-shared";
  const auto node = [](std::string_view role) {
    const std::string prefix = std::string(role) + "/";
    const std::string audio_filename =
        role == "face_on" ? "audio_evidence.wav" : "diagnostic_audio.wav";
    return Json{
        {"passed", true},
        {"node_id", role == "face_on" ? "leader-node" : "shadow-node"},
        {"role", role},
        {"shared_session_id", "pose-hil-shared"},
        {"startup_timing",
         {{"schema_version", 1},
          {"clock", "CLOCK_BOOTTIME"},
          {"continuity_clean", true},
          {"startup_continuity_reset_count", "0"},
          {"maximum_startup_continuity_gap_ns", "0"},
          {"milestones",
           {{"arm_requested_elapsed_realtime_ns", "100"},
            {"engine_started_elapsed_realtime_ns", "120"},
            {"first_camera_frame_elapsed_realtime_ns", "210"},
            {"first_usable_encoded_frame_elapsed_realtime_ns", "260"},
            {"full_pre_roll_ready_elapsed_realtime_ns", "2800"}}},
          {"durations",
           {{"arm_to_engine_start_ns", "20"},
            {"engine_start_to_first_camera_frame_ns", "90"},
            {"arm_to_first_camera_frame_ns", "110"},
            {"first_camera_frame_to_first_usable_encoded_frame_ns", "50"},
            {"arm_to_first_usable_encoded_frame_ns", "160"},
            {"first_usable_encoded_frame_to_full_pre_roll_ready_ns", "2540"},
            {"arm_to_full_pre_roll_ready_ns", "2700"}}}}},
        {"trigger_timestamp_uncertainty_ns", 400000},
        {"audio", {{"retained_wav", prefix + audio_filename}}},
        {"decoded_video",
         {{"passed", true}, {"exact_frame_count", true}, {"maximum_media_time_residual_us", 200}}},
        {"optical",
         {{"passed", true},
          {"optical_to_audio_offset_us", -15000},
          {"timing_correlation",
           {{"passed", true},
            {"claim_scope", "fixture_optical_marker_to_audio_trigger"},
            {"absolute_ball_impact_calibrated", false},
            {"acceptance_limit_us", 25000},
            {"optical_onset_lower_bound_us", -19000},
            {"optical_onset_upper_bound_us", -11000},
            {"optical_interval_width_us", 8000},
            {"audio_trigger_uncertainty_us", 400},
            {"media_pts_residual_us", 200},
            {"accounted_uncertainty_us", 600},
            {"minimum_residual_us", -19600},
            {"maximum_residual_us", -10400},
            {"total_bound_us", 19600}}}}},
        {"report", prefix + "report.json"},
        {"manifest", prefix + "manifest.json"},
        {"media", prefix + std::string(role) + ".mp4"},
        {"audio_evidence", prefix + audio_filename},
        {"ffprobe", prefix + "ffprobe.json"},
        {"diagnostic_frames",
         {{"pre", prefix + "diagnostic-01.png"},
          {"marker", prefix + "diagnostic-02.png"},
          {"post", prefix + "diagnostic-03.png"}}}};
  };
  const auto artifacts = [](std::string_view role) {
    const std::string prefix = std::string(role) + "/";
    Json result = {
        {"initial_node_descriptor", prefix + "node-descriptor-initial.json"},
        {"pose_configured_node_descriptor", prefix + "node-descriptor-pose-configured.json"},
        {"pose_setup", prefix + "pose-setup.json"},
        {"pose_status_monitoring", prefix + "pose-status-monitoring.json"},
        {"pose_status_high_speed", prefix + "pose-status-high_speed.json"},
        {"setup_preview", prefix + "setup-preview.jpg"},
        {"setup_preview_metadata", prefix + "setup-preview.json"},
        {"setup_preview_high_speed", prefix + "setup-preview-high-speed.json"},
        {"capture",
         {{"capture_report", prefix + "report.json"},
          {"manifest", prefix + "manifest.json"},
          {"media", prefix + std::string(role) + ".mp4"},
          {"audio", prefix + (role == "face_on" ? "audio_evidence.wav" : "diagnostic_audio.wav")},
          {"ffprobe", prefix + "ffprobe.json"},
          {"diagnostic_frames",
           {{"pre", prefix + "diagnostic-01.png"},
            {"marker", prefix + "diagnostic-02.png"},
            {"post", prefix + "diagnostic-03.png"}}}}},
    };
    if (role == "down_the_line") {
      result["pose_status_triggered"] = prefix + "pose-status-triggered.json";
    }
    result["lan_node_descriptor"] = prefix + "lan-node-descriptor.json";
    result["lan_setup_authenticated"] = prefix + "lan-setup-authenticated.json";
    result["lan_setup_unauthenticated"] = prefix + "lan-setup-unauthenticated.json";
    result["lan_capture_status"] = prefix + "lan-capture-status.json";
    result["lan_clock"] = prefix + "lan-clock.json";
    if (role == "face_on") {
      result["pair_network_health_accepted"] = prefix + "pair-network-health-accepted.json";
    }
    return result;
  };
  Json network_health = PairNetworkStatusFixture("good").at("pair_network_health");
  network_health["peer"]["origin"] = "http://10.0.0.5:8088";
  return Json{
      {"schema_version", 1},
      {"report_type", "android_dual_phone_paired_pose_arm_lan_hil"},
      {"passed", true},
      {"camera_jobs_concurrent", true},
      {"single_pcm_replay_count", 1},
      {"shared_session_id", kSharedSessionId},
      {"timing_claim",
       {{"scope", "fixture_optical_marker_to_audio_trigger"},
        {"qualification", "operational_correlation_only"},
        {"review_marker_semantics", "audio_trigger_estimate"},
        {"absolute_ball_impact_calibrated", false},
        {"unmeasured_latency_components",
         Json::array({"camera_exposure_timestamp_semantics", "microphone_input_path_latency",
                      "speaker_amplifier_and_fixture_acoustic_latency",
                      "field_ball_to_microphone_acoustic_latency"})}}},
      {"pose_transition",
       {{"passed", true},
        {"leader_role", "face_on"},
        {"shadow_role", "down_the_line"},
        {"peer_dispatch", "production_pose_peer_arm_client"},
        {"peer_transport", "wifi_lan_direct"},
        {"adb_reverse_used", false},
        {"host_control_transport", "adb_forward"},
        {"high_speed_profile", "720p240"},
        {"leader_trigger_source", "local_audio"},
        {"shadow_trigger_source", "peer_audio_clock_candidate"},
        {"peer_impact_schema_version", 2},
        {"target_peer_node_id_verified", true},
        {"mapped_fallback_semantics", "mapped_candidate_else_arrival"},
        {"mapped_impact_evidence", {{"shared_session_id", kSharedSessionId}}},
        {"endpoint_hil_launch_gated", true},
        {"persisted_peer_arm",
         {{"leader", {{"state", "accepted"}, {"shared_session_id", kSharedSessionId}}},
          {"shadow", {{"state", "inbound_accepted"}, {"shared_session_id", kSharedSessionId}}}}}}},
      {"configuration_restore",
       {{"passed", true},
        {"scope", "complete_private_node_configuration_generation"},
        {"temporary_peer_configuration_removed", true},
        {"secret_material_preserved_in_artifacts", false}}},
      {"standby_inference_validation",
       {{"down_the_line",
         {{"passed", true},
          {"actual_delegate", "gpu"},
          {"successful_inferences", 4},
          {"inference_duration_p95_ns", 161'000'000},
          {"maximum_inference_duration_ns", 160'000'000},
          {"inference_deadline_misses", 0},
          {"inference_outliers", 0},
          {"decision_age_samples", 4},
          {"rejected_decision_timestamps", 0},
          {"decision_age_p95_ns", 393'000'000},
          {"maximum_decision_age_ns", 393'000'000},
          {"offered_images", 9},
          {"scheduled_images", 8},
          {"dropped_images", 3},
          {"maximum_warmup_duration_ns", 791'000'000}}},
        {"face_on",
         {{"passed", true},
          {"actual_delegate", "gpu"},
          {"successful_inferences", 7},
          {"inference_duration_p95_ns", 135'000'000},
          {"maximum_inference_duration_ns", 134'000'000},
          {"inference_deadline_misses", 0},
          {"inference_outliers", 0},
          {"decision_age_samples", 7},
          {"rejected_decision_timestamps", 0},
          {"decision_age_p95_ns", 214'000'000},
          {"maximum_decision_age_ns", 213'000'000},
          {"offered_images", 10},
          {"scheduled_images", 9},
          {"dropped_images", 1},
          {"maximum_warmup_duration_ns", 138'000'000}}}}},
      {"setup_preview_validation",
       {{"down_the_line",
         {{"monitoring",
           {{"passed", true},
            {"unauthenticated_status", 401},
            {"authenticated_status", 200},
            {"content_type", "image/jpeg"},
            {"cache_control", "no-store, max-age=0"},
            {"byte_count", 1024},
            {"state", "available"},
            {"reason", "none"},
            {"generation", 2},
            {"image_rotation_degrees", 90},
            {"frame_age_ms", 150}}},
          {"high_speed",
           {{"passed", true},
            {"authenticated_status", 503},
            {"cache_control", "no-store, max-age=0"},
            {"state", "unavailable"},
            {"reason", "high_speed_capture"}}}}},
        {"face_on",
         {{"monitoring",
           {{"passed", true},
            {"unauthenticated_status", 401},
            {"authenticated_status", 200},
            {"content_type", "image/jpeg"},
            {"cache_control", "no-store, max-age=0"},
            {"byte_count", 1024},
            {"state", "available"},
            {"reason", "none"},
            {"generation", 3},
            {"image_rotation_degrees", 90},
            {"frame_age_ms", 200}}},
          {"high_speed",
           {{"passed", true},
            {"authenticated_status", 503},
            {"cache_control", "no-store, max-age=0"},
            {"state", "unavailable"},
            {"reason", "high_speed_capture"}}}}}}},
      {"pair_network_health_admission",
       {{"schema_version", 1},
        {"passed", true},
        {"poll_count", 2},
        {"elapsed_milliseconds", 250},
        {"accepted_state", "good"},
        {"degraded_override", false},
        {"expected_peer_origin", "http://10.0.0.5:8088"},
        {"expected_peer_node_id", "shadow-node"},
        {"snapshot", network_health}}},
      {"lan_endpoint_validation",
       {{"down_the_line",
         {{"host_direct_request", true},
          {"unauthenticated_setup_status", 401},
          {"authenticated_setup_status", 200},
          {"descriptor_schema_version", 1},
          {"status_schema_version", 2},
          {"clock_schema_version", 1},
          {"control_authentication", "bearer"},
          {"secret_material_preserved", false},
          {"role", "down_the_line"},
          {"pose_mode", "shadow"},
          {"origin", "http://10.0.0.5:8088"},
          {"peer_origin", nullptr}}},
        {"face_on",
         {{"host_direct_request", true},
          {"unauthenticated_setup_status", 401},
          {"authenticated_setup_status", 200},
          {"descriptor_schema_version", 1},
          {"status_schema_version", 2},
          {"clock_schema_version", 1},
          {"control_authentication", "bearer"},
          {"secret_material_preserved", false},
          {"role", "face_on"},
          {"pose_mode", "leader"},
          {"origin", "http://10.0.0.6:8088"},
          {"peer_origin", "http://10.0.0.5:8088"}}}}},
      {"artifacts",
       {{"down_the_line", artifacts("down_the_line")}, {"face_on", artifacts("face_on")}}},
      {"nodes", Json::array({node("down_the_line"), node("face_on")})},
  };
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

void DiscoveryPairingFixtureHandlesPreservedFieldState() {
  Json setup = {
      {"schema_version", 1},
      {"revision", 17},
      {"configuration",
       {{"pose", {{"mode", "leader"}, {"peer", {{"origin", "http://10.0.0.5:8088"}}}}}}},
      {"pairing", {{"state", "active"}, {"peer_node_id", "field-shadow"}}},
  };
  auto inspection = InspectDiscoveryPairingFixture(setup.dump());
  assert(inspection.peer_configured);
  assert(inspection.pairing_state == DurablePairingState::kActive);
  assert(inspection.peer_node_id == "field-shadow");
  assert(!inspection.IsUnpaired());

  setup["configuration"]["pose"]["peer"] = nullptr;
  setup["configuration"]["pose"]["mode"] = "disabled";
  setup["pairing"]["state"] = "revoked";
  inspection = InspectDiscoveryPairingFixture(setup.dump());
  assert(!inspection.peer_configured);
  assert(inspection.pairing_state == DurablePairingState::kRevoked);
  assert(!inspection.IsUnpaired());

  setup["pairing"] = nullptr;
  inspection = InspectDiscoveryPairingFixture(setup.dump());
  assert(inspection.IsUnpaired());

  setup["configuration"]["pose"]["mode"] = "shadow";
  inspection = InspectDiscoveryPairingFixture(setup.dump());
  assert(!inspection.IsUnpaired());
  setup["configuration"]["pose"]["mode"] = "disabled";

  setup["pairing"] = {{"state", "unknown"}, {"peer_node_id", "field-shadow"}};
  ExpectFailure([&] { static_cast<void>(InspectDiscoveryPairingFixture(setup.dump())); });
}

void ShortPoseLatencyCatchesARealFiveHertzRegression() {
  Json status = {
      {"pose",
       {{"metrics",
         {{"delegate", "gpu"},
          {"offered_images", 10},
          {"scheduled_images", 9},
          {"dropped_images", 1},
          {"successful_warmup_inferences", 1},
          {"failed_warmup_inferences", 0},
          {"maximum_warmup_duration_ns", 714'000'000},
          {"successful_inferences", 7},
          {"failed_inferences", 0},
          {"maximum_inference_duration_ns", 180'000'000},
          {"inference_duration_p95_ns", 175'000'000},
          {"inference_deadline_misses", 0},
          {"inference_outliers", 0},
          {"decision_age_samples", 7},
          {"rejected_decision_timestamps", 0},
          {"maximum_decision_age_ns", 260'000'000},
          {"decision_age_p95_ns", 240'000'000}}}}},
  };
  const auto evidence = ValidateShortPoseLatency(status.dump());
  assert(evidence.actual_delegate == "gpu");
  assert(evidence.successful_inferences == 7);
  assert(evidence.maximum_warmup_duration_ns == 714'000'000);

  status["pose"]["metrics"]["inference_duration_p95_ns"] = 200'000'001;
  ExpectFailure([&] { static_cast<void>(ValidateShortPoseLatency(status.dump())); });
  status["pose"]["metrics"]["inference_duration_p95_ns"] = 175'000'000;
  status["pose"]["metrics"]["maximum_inference_duration_ns"] = 400'000'001;
  ExpectFailure([&] { static_cast<void>(ValidateShortPoseLatency(status.dump())); });
  status["pose"]["metrics"]["maximum_inference_duration_ns"] = 180'000'000;
  status["pose"]["metrics"]["rejected_decision_timestamps"] = 1;
  ExpectFailure([&] { static_cast<void>(ValidateShortPoseLatency(status.dump())); });
  status["pose"]["metrics"]["rejected_decision_timestamps"] = 0;
  status["pose"]["metrics"]["decision_age_samples"] = 6;
  ExpectFailure([&] { static_cast<void>(ValidateShortPoseLatency(status.dump())); });
  status["pose"]["metrics"]["decision_age_samples"] = 7;
  status["pose"]["metrics"]["delegate"] = "cpu";
  ExpectFailure([&] { static_cast<void>(ValidateShortPoseLatency(status.dump())); });
}

void PoseConfiguredDescriptorIsStageExact() {
  const NodeApiIdentity identity = Identity();
  Json descriptor = {
      {"schema_version", 1},
      {"node_id", "node-dtl"},
      {"role", "down_the_line"},
      {"capture_profile", "720p240"},
      {"pose",
       {{"mode", "leader"},
        {"delegate", "gpu_preferred"},
        {"debug_evidence_enabled", true},
        {"peer_configured", true}}},
  };
  const auto inspection = [&] {
    ValidatePoseConfiguredNodeDescriptor(PoseConfiguredDescriptorInspection{
        .descriptor_json = descriptor.dump(),
        .identity = identity,
        .expected_mode = "leader",
        .expected_peer_configured = true,
    });
  };
  inspection();
  descriptor["pose"]["mode"] = "shadow";
  ExpectFailure(inspection);
}

void PairedPoseReportRequiresStageExactEvidence() {
  Json report = PairedPoseReportFixture();
  ValidatePairedPoseHilReport(report.dump());

  Json stale_descriptor = report;
  stale_descriptor["artifacts"]["down_the_line"].erase("initial_node_descriptor");
  stale_descriptor["artifacts"]["down_the_line"]["node_descriptor"] =
      "down_the_line/node-descriptor.json";
  ExpectFailure([&] { ValidatePairedPoseHilReport(stale_descriptor.dump()); });

  Json missing_monitoring = report;
  missing_monitoring["artifacts"]["face_on"].erase("pose_status_monitoring");
  ExpectFailure([&] { ValidatePairedPoseHilReport(missing_monitoring.dump()); });

  Json mismatched_session = report;
  mismatched_session["pose_transition"]["persisted_peer_arm"]["shadow"]["shared_session_id"] =
      "other-session";
  ExpectFailure([&] { ValidatePairedPoseHilReport(mismatched_session.dump()); });

  Json stale_preview = report;
  stale_preview["setup_preview_validation"]["face_on"]["monitoring"]["state"] = "stale";
  ExpectFailure([&] { ValidatePairedPoseHilReport(stale_preview.dump()); });

  Json preview_leaked_into_high_speed = report;
  preview_leaked_into_high_speed["setup_preview_validation"]["down_the_line"]["high_speed"]
                                ["authenticated_status"] = 200;
  ExpectFailure([&] { ValidatePairedPoseHilReport(preview_leaked_into_high_speed.dump()); });

  Json slow_inference = report;
  slow_inference["standby_inference_validation"]["down_the_line"]["inference_duration_p95_ns"] =
      200'000'001;
  ExpectFailure([&] { ValidatePairedPoseHilReport(slow_inference.dump()); });

  Json missing_capture_media = report;
  missing_capture_media["artifacts"]["down_the_line"]["capture"].erase("media");
  ExpectFailure([&] { ValidatePairedPoseHilReport(missing_capture_media.dump()); });

  Json mismatched_node_audio = report;
  mismatched_node_audio["nodes"][0]["audio_evidence"] = "face_on/wrong.wav";
  ExpectFailure([&] { ValidatePairedPoseHilReport(mismatched_node_audio.dump()); });

  Json inconsistent_startup = report;
  inconsistent_startup["nodes"][0]["startup_timing"]["durations"]["arm_to_first_camera_frame_ns"] =
      "111";
  ExpectFailure([&] { ValidatePairedPoseHilReport(inconsistent_startup.dump()); });

  Json startup_gap = report;
  startup_gap["nodes"][1]["startup_timing"]["continuity_clean"] = false;
  startup_gap["nodes"][1]["startup_timing"]["startup_continuity_reset_count"] = "1";
  startup_gap["nodes"][1]["startup_timing"]["maximum_startup_continuity_gap_ns"] = "5000000";
  ExpectFailure([&] { ValidatePairedPoseHilReport(startup_gap.dump()); });

  Json overstated_timing_claim = report;
  overstated_timing_claim["timing_claim"]["absolute_ball_impact_calibrated"] = true;
  ExpectFailure([&] { ValidatePairedPoseHilReport(overstated_timing_claim.dump()); });

  Json dropped_latency_component = report;
  dropped_latency_component["timing_claim"]["unmeasured_latency_components"].erase(0);
  ExpectFailure([&] { ValidatePairedPoseHilReport(dropped_latency_component.dump()); });

  Json unpropagated_audio_uncertainty = report;
  unpropagated_audio_uncertainty["nodes"][0]["optical"]["timing_correlation"]
                                ["audio_trigger_uncertainty_us"] = 0;
  ExpectFailure([&] { ValidatePairedPoseHilReport(unpropagated_audio_uncertainty.dump()); });

  Json falsified_timing_bound = report;
  falsified_timing_bound["nodes"][1]["optical"]["timing_correlation"]["total_bound_us"] = 1;
  ExpectFailure([&] { ValidatePairedPoseHilReport(falsified_timing_bound.dump()); });

  Json weakened_timing_limit = report;
  weakened_timing_limit["nodes"][0]["optical"]["timing_correlation"]["acceptance_limit_us"] =
      100000;
  ExpectFailure([&] { ValidatePairedPoseHilReport(weakened_timing_limit.dump()); });

  Json retired_tunnel_contract = report;
  retired_tunnel_contract["report_type"] = "android_dual_phone_paired_pose_arm_hil";
  retired_tunnel_contract["pose_transition"]["peer_transport"] = "adb_reverse_to_shadow_http_api";
  retired_tunnel_contract["pose_transition"]["adb_reverse_used"] = true;
  ExpectFailure([&] { ValidatePairedPoseHilReport(retired_tunnel_contract.dump()); });

  Json missing_health = report;
  missing_health.erase("pair_network_health_admission");
  ExpectFailure([&] { ValidatePairedPoseHilReport(missing_health.dump()); });

  Json mismatched_health_peer = report;
  mismatched_health_peer["pair_network_health_admission"]["expected_peer_node_id"] = "other-shadow";
  ExpectFailure([&] { ValidatePairedPoseHilReport(mismatched_health_peer.dump()); });

  Json undeclared_override = report;
  undeclared_override["pair_network_health_admission"]["degraded_override"] = true;
  ExpectFailure([&] { ValidatePairedPoseHilReport(undeclared_override.dump()); });

  Json reverse_transport = report;
  reverse_transport["pose_transition"]["adb_reverse_used"] = true;
  ExpectFailure([&] { ValidatePairedPoseHilReport(reverse_transport.dump()); });
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
  const auto trigger =
      ValidateTriggerReport(report.dump(), identity, "shared-1", "local-dtl", "local_audio");
  assert(trigger.trigger_timestamp_ns == 123456789L);
  report["local_session_id"] = "foreign";
  ExpectFailure([&] {
    static_cast<void>(
        ValidateTriggerReport(report.dump(), identity, "shared-1", "local-dtl", "local_audio"));
  });
  report["local_session_id"] = "local-dtl";
  report["source"] = "manual";
  ExpectFailure([&] {
    static_cast<void>(
        ValidateTriggerReport(report.dump(), identity, "shared-1", "local-dtl", "local_audio"));
  });
}

void ProductionClockAndMappedImpactEvidenceAreExact() {
  const NodeApiIdentity leader{.node_id = "node-face",
                               .role = coordination::CaptureRole::kFaceOn,
                               .capture_profile = "720p240"};
  const NodeApiIdentity shadow{.node_id = "node-dtl",
                               .role = coordination::CaptureRole::kDownTheLine,
                               .capture_profile = "720p240"};
  Json leader_status = {{"pose",
                         {{"mode", "leader"},
                          {"peer_configured", true},
                          {"peer_clock",
                           {{"peer_node_id", "node-dtl"},
                            {"age_ns", "1000"},
                            {"uncertainty_ns", "2000"},
                            {"minimum_round_trip_ns", "3000"},
                            {"maximum_round_trip_ns", "4000"},
                            {"sample_count", 3}}}}}};
  Json shadow_status = {
      {"pose", {{"mode", "shadow"}, {"peer_configured", false}, {"peer_clock", nullptr}}}};
  ValidatePairedPoseClockStatus({.leader_status_json = leader_status.dump(),
                                 .shadow_status_json = shadow_status.dump(),
                                 .leader_identity = leader,
                                 .shadow_identity = shadow});
  leader_status["pose"]["peer_clock"]["uncertainty_ns"] = "25000000";
  ValidatePairedPoseClockStatus({.leader_status_json = leader_status.dump(),
                                 .shadow_status_json = shadow_status.dump(),
                                 .leader_identity = leader,
                                 .shadow_identity = shadow});
  leader_status["pose"]["peer_clock"]["uncertainty_ns"] = "25000001";
  ExpectFailure([&] {
    ValidatePairedPoseClockStatus({.leader_status_json = leader_status.dump(),
                                   .shadow_status_json = shadow_status.dump(),
                                   .leader_identity = leader,
                                   .shadow_identity = shadow});
  });
  leader_status["pose"]["peer_clock"]["uncertainty_ns"] = "2000";

  shadow_status["pose"]["peer_impact_mapping"] = {
      {"schema_version", 2},
      {"leader_node_id", "node-face"},
      {"target_peer_node_id", "node-dtl"},
      {"shared_session_id", "shared-1"},
      {"mapped_peer_trigger_elapsed_realtime_ns", "5000"},
      {"mapping_uncertainty_ns", "2000"},
      {"mapping_age_at_send_ns", "1000"},
      {"minimum_round_trip_ns", "3000"},
      {"maximum_round_trip_ns", "4000"},
      {"sample_count", 3},
      {"mapping_within_policy", true},
      {"selected_source", "peer_audio_clock_candidate"},
      {"fallback_semantics", "mapped_candidate_else_arrival"},
  };
  const auto mapped_inspection = [&] {
    ValidateMappedPeerImpactStatus({.shadow_status_json = shadow_status.dump(),
                                    .leader_identity = leader,
                                    .shadow_identity = shadow,
                                    .expected_shared_session_id = "shared-1"});
  };
  mapped_inspection();
  shadow_status["pose"]["peer_impact_mapping"]["mapping_uncertainty_ns"] = "25000000";
  mapped_inspection();
  shadow_status["pose"]["peer_impact_mapping"]["mapping_uncertainty_ns"] = "25000001";
  ExpectFailure(mapped_inspection);
  shadow_status["pose"]["peer_impact_mapping"]["mapping_uncertainty_ns"] = "2000";
  leader_status["last_trigger_elapsed_realtime_ns"] = "6000";
  shadow_status["last_trigger_elapsed_realtime_ns"] = "5000";
  assert(HasPairedAutomaticImpactEvidence(leader_status.dump(), shadow_status.dump()));
  shadow_status["last_trigger_elapsed_realtime_ns"] = nullptr;
  assert(!HasPairedAutomaticImpactEvidence(leader_status.dump(), shadow_status.dump()));
  shadow_status["last_trigger_elapsed_realtime_ns"] = "5000";
  shadow_status["pose"]["peer_impact_mapping"] = nullptr;
  assert(!HasPairedAutomaticImpactEvidence(leader_status.dump(), shadow_status.dump()));
  shadow_status["pose"]["peer_impact_mapping"] = {
      {"schema_version", 2},
      {"leader_node_id", "node-face"},
      {"target_peer_node_id", "node-dtl"},
      {"shared_session_id", "shared-1"},
      {"mapped_peer_trigger_elapsed_realtime_ns", "5000"},
      {"mapping_uncertainty_ns", "2000"},
      {"mapping_age_at_send_ns", "1000"},
      {"minimum_round_trip_ns", "3000"},
      {"maximum_round_trip_ns", "4000"},
      {"sample_count", 3},
      {"mapping_within_policy", true},
      {"selected_source", "peer_audio_clock_candidate"},
      {"fallback_semantics", "mapped_candidate_else_arrival"},
  };
  shadow_status["pose"]["peer_impact_mapping"]["target_peer_node_id"] = "other";
  ExpectFailure(mapped_inspection);
  shadow_status["pose"]["peer_impact_mapping"]["target_peer_node_id"] = "node-dtl";
  shadow_status["pose"]["peer_impact_mapping"]["shared_session_id"] = "stale-session";
  ExpectFailure(mapped_inspection);
}

Json PoseSessionStatus(std::string_view mode, std::string_view phase,
                       std::string_view shared_session_id) {
  const bool high_speed = phase == "high_speed";
  const bool leader = mode == "leader";
  return {
      {"schema_version", 2},
      {"state", "armed"},
      {"armed", true},
      {"error", ""},
      {"shared_session_id", high_speed ? Json(shared_session_id) : Json(nullptr)},
      {"last_trigger_elapsed_realtime_ns", nullptr},
      {"ring_duration_us", high_speed ? 2'000'000 : 0},
      {"pose",
       {{"mode", mode},
        {"phase", phase},
        {"peer_configured", leader},
        {"transition_requested", high_speed},
        {"standby_audio", {{"ready", !high_speed}}},
        {"high_speed_audio", {{"ready", high_speed}}},
        {"peer_arm",
         {{"state", high_speed ? (leader ? "accepted" : "inbound_accepted") : "not_requested"},
          {"shared_session_id", high_speed ? Json(shared_session_id) : Json(nullptr)},
          {"http_status", high_speed && leader ? Json(202) : Json(nullptr)},
          {"failure_type", nullptr}}}}},
  };
}

void PairedPoseSessionStateModelIsExact() {
  Json leader = PoseSessionStatus("leader", "monitoring", "");
  Json shadow = PoseSessionStatus("shadow", "monitoring", "");
  ValidatePairedPoseSessionState({.leader_status_json = leader.dump(),
                                  .shadow_status_json = shadow.dump(),
                                  .expected_phase = "monitoring",
                                  .expected_shared_session_id = {}});
  leader = PoseSessionStatus("leader", "high_speed", "shared-1");
  shadow = PoseSessionStatus("shadow", "high_speed", "shared-1");
  const auto validate = [&] {
    ValidatePairedPoseSessionState({.leader_status_json = leader.dump(),
                                    .shadow_status_json = shadow.dump(),
                                    .expected_phase = "high_speed",
                                    .expected_shared_session_id = "shared-1"});
  };
  validate();
  shadow["pose"]["peer_arm"]["shared_session_id"] = "other";
  ExpectFailure(validate);
  shadow = PoseSessionStatus("shadow", "high_speed", "shared-1");
  shadow["pose"]["high_speed_audio"]["ready"] = false;
  ExpectFailure(validate);
}

Json PairNetworkDirectionFixture() {
  return {
      {"schema_version", 1},
      {"attempts", 3},
      {"successes", 3},
      {"timeouts", 0},
      {"round_trip_ns", Json::array({"1000000", "2000000", "3000000"})},
      {"transfer_bytes", "1048576"},
      {"transfer_duration_ns", "100000000"},
      {"transfer_complete", true},
      {"minimum_round_trip_ns", "1000000"},
      {"median_round_trip_ns", "2000000"},
      {"p95_round_trip_ns", "3000000"},
      {"maximum_round_trip_ns", "3000000"},
      {"jitter_ns", "2000000"},
      {"transfer_bits_per_second", 83'886'080.0},
  };
}

Json PairNetworkStatusFixture(std::string_view state) {
  const bool measured = state != "unknown";
  const std::string visible_state = measured ? std::string(state) : "unusable";
  return {
      {"schema_version", 2},
      {"pair_network_health",
       {{"schema_version", 1},
        {"configured", true},
        {"state", visible_state},
        {"raw_state", visible_state},
        {"measured", measured},
        {"stale", false},
        {"transition_pending", false},
        {"age_ns", measured ? "1000000" : "0"},
        {"issues", visible_state == "good" ? Json::array()
                                           : Json::array({"pair network health is not good"})},
        {"peer", {{"origin", "http://10.168.168.241:8088"}, {"node_id", "shadow-node"}}},
        {"measured_at_elapsed_realtime_ns", measured ? Json("9000000") : Json(nullptr)},
        {"local_to_peer", measured ? PairNetworkDirectionFixture() : Json(nullptr)},
        {"peer_to_local", measured ? PairNetworkDirectionFixture() : Json(nullptr)}}},
  };
}

void PairNetworkHealthArmBoundaryIsFailClosed() {
  auto inspect = [](const Json &status) {
    return InspectPairNetworkHealthForArm(PairNetworkHealthStatusInspection{
        .status_json = status.dump(),
        .expected_peer_origin = "http://10.168.168.241:8088",
        .expected_peer_node_id = "shadow-node",
    });
  };

  Json status = PairNetworkStatusFixture("good");
  assert(inspect(status) == PairNetworkHealthArmDecision::kGood);
  Json request = Json::parse(PairNetworkHealthArmRequestBody(PairNetworkHealthArmDecision::kGood));
  assert((request == Json{{"armed", true}}));

  status = PairNetworkStatusFixture("degraded");
  assert(inspect(status) == PairNetworkHealthArmDecision::kDegraded);
  request = Json::parse(PairNetworkHealthArmRequestBody(PairNetworkHealthArmDecision::kDegraded));
  assert(request == Json({{"armed", true}, {"allow_degraded_network", true}}));

  status = PairNetworkStatusFixture("unusable");
  assert(inspect(status) == PairNetworkHealthArmDecision::kNotReady);
  ExpectFailure([] {
    static_cast<void>(PairNetworkHealthArmRequestBody(PairNetworkHealthArmDecision::kNotReady));
  });

  status = PairNetworkStatusFixture("unknown");
  assert(inspect(status) == PairNetworkHealthArmDecision::kNotReady);
  status = PairNetworkStatusFixture("good");
  status["pair_network_health"]["stale"] = true;
  ExpectFailure([&] { static_cast<void>(inspect(status)); });
  status["pair_network_health"]["state"] = "unusable";
  status["pair_network_health"]["raw_state"] = "unusable";
  status["pair_network_health"]["issues"] = Json::array({"evidence is stale"});
  assert(inspect(status) == PairNetworkHealthArmDecision::kNotReady);
  status = PairNetworkStatusFixture("good");
  status["pair_network_health"]["transition_pending"] = true;
  status["pair_network_health"]["raw_state"] = "degraded";
  status["pair_network_health"]["issues"] = Json::array({"degradation is pending"});
  assert(inspect(status) == PairNetworkHealthArmDecision::kGood);
  status = PairNetworkStatusFixture("degraded");
  status["pair_network_health"]["transition_pending"] = true;
  status["pair_network_health"]["raw_state"] = "good";
  assert(inspect(status) == PairNetworkHealthArmDecision::kDegraded);
  status = PairNetworkStatusFixture("unusable");
  status["pair_network_health"]["transition_pending"] = true;
  status["pair_network_health"]["raw_state"] = "good";
  assert(inspect(status) == PairNetworkHealthArmDecision::kNotReady);
  status["pair_network_health"]["transition_pending"] = false;
  ExpectFailure([&] { static_cast<void>(inspect(status)); });
  status = PairNetworkStatusFixture("good");
  status["pair_network_health"]["peer"]["origin"] = "http://10.168.168.111:8088";
  assert(inspect(status) == PairNetworkHealthArmDecision::kNotReady);

  status = PairNetworkStatusFixture("unknown");
  status["pair_network_health"]["age_ns"] = "1";
  ExpectFailure([&] { static_cast<void>(inspect(status)); });
  status = PairNetworkStatusFixture("unknown");
  status["pair_network_health"]["configured"] = false;
  status["pair_network_health"]["peer"] = nullptr;
  ExpectFailure([&] { static_cast<void>(inspect(status)); });

  status = PairNetworkStatusFixture("good");
  status["pair_network_health"]["age_ns"] = 1;
  ExpectFailure([&] { static_cast<void>(inspect(status)); });
  status = PairNetworkStatusFixture("good");
  status["pair_network_health"]["local_to_peer"]["round_trip_ns"] = Json::array({"1000000"});
  ExpectFailure([&] { static_cast<void>(inspect(status)); });
  status = PairNetworkStatusFixture("good");
  status["pair_network_health"]["local_to_peer"]["round_trip_ns"] =
      Json::array({"1000000", "3000000", "2000000"});
  ExpectFailure([&] { static_cast<void>(inspect(status)); });
  status = PairNetworkStatusFixture("good");
  status["pair_network_health"]["local_to_peer"]["median_round_trip_ns"] = "3000000";
  ExpectFailure([&] { static_cast<void>(inspect(status)); });
  status = PairNetworkStatusFixture("good");
  status["pair_network_health"]["local_to_peer"]["transfer_bits_per_second"] = 1.0;
  ExpectFailure([&] { static_cast<void>(inspect(status)); });
  status = PairNetworkStatusFixture("good");
  status["pair_network_health"]["issues"] = Json::array({"not actually good"});
  ExpectFailure([&] { static_cast<void>(inspect(status)); });
  status = PairNetworkStatusFixture("good");
  status["pair_network_health"].erase("issues");
  ExpectFailure([&] { static_cast<void>(inspect(status)); });
}

void LanEndpointContractRequiresAdvertisedDirectOriginAndBearerAuth() {
  const NodeApiIdentity identity = Identity();
  constexpr std::string_view kOrigin = "http://10.168.168.241:8088";
  Json descriptor = {
      {"schema_version", 1},
      {"node_id", "node-dtl"},
      {"role", "down_the_line"},
      {"capture_profile", "720p240"},
      {"service_urls", Json::array({kOrigin})},
      {"control_authentication", "bearer"},
  };
  Json setup = {
      {"schema_version", 1},
      {"node",
       {{"node_id", "node-dtl"},
        {"service_urls", Json::array({kOrigin})},
        {"device_model", "Pixel 5a"}}},
      {"configuration",
       {{"role", "down_the_line"},
        {"capture_profile", "720p240"},
        {"pose", {{"mode", "shadow"}, {"peer", nullptr}}}}},
      {"readiness", {{"editable", true}, {"issues", Json::array()}}},
  };
  const Json status = {{"schema_version", 2}};
  const Json clock = {
      {"schema_version", 1},
      {"node_id", "node-dtl"},
      {"request_received_elapsed_realtime_ns", "1000"},
      {"response_prepared_elapsed_realtime_ns", "2000"},
  };
  const auto validate = [&] {
    ValidateLanEndpoint({.descriptor_json = descriptor.dump(),
                         .setup_json = setup.dump(),
                         .status_json = status.dump(),
                         .clock_json = clock.dump(),
                         .identity = identity,
                         .expected_origin = kOrigin,
                         .expected_device_model = "Pixel 5a",
                         .expected_pose_mode = "shadow",
                         .expected_peer_origin = {},
                         .unauthenticated_setup_status = 401,
                         .authenticated_setup_status = 200});
  };
  validate();
  descriptor["service_urls"] = Json::array({"http://127.0.0.1:8088"});
  ExpectFailure(validate);
  descriptor["service_urls"] = Json::array({kOrigin});
  setup["control_token"] = "secret";
  ExpectFailure(validate);
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
  DiscoveryPairingFixtureHandlesPreservedFieldState();
  ShortPoseLatencyCatchesARealFiveHertzRegression();
  PoseConfiguredDescriptorIsStageExact();
  PairedPoseReportRequiresStageExactEvidence();
  IdentityAndSessionMismatchFail();
  ExactTriggerReportPassesAndCorruptionFails();
  ProductionClockAndMappedImpactEvidenceAreExact();
  PairedPoseSessionStateModelIsExact();
  PairNetworkHealthArmBoundaryIsFailClosed();
  LanEndpointContractRequiresAdvertisedDirectOriginAndBearerAuth();
  CanonicalReplayIsByteExactExceptForFramingNewline();
  return 0;
}
