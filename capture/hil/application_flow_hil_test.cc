#include <httplib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "capture/hil/feather_hil_controller.h"
#include "capture/hil/feather_hil_serial.h"
#include "capture/hil/session_artifact_validator.h"
#include "capture/service/preview_api.h"
#include "capture/service/preview_station.h"
#include "station/hardware_lock.h"
#include "station/station_config.h"

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
using swing_capture::hil::FeatherDeviceInfo;
using swing_capture::hil::FeatherHilController;
using swing_capture::hil::FeatherHilSerial;
using swing_capture::hil::SessionArtifactExpectations;
using swing_capture::hil::SessionArtifactValidation;
using swing_capture::hil::ValidateSessionArtifacts;
using swing_capture::service::PreviewStation;
using swing_capture::station::HardwareLock;
using swing_capture::station::StationConfig;

constexpr auto kWorkflowDeadline = 15s;
constexpr auto kHttpTimeout = 2s;
constexpr double kExpectedHilExposureMicroseconds = 500.0;
constexpr double kExpectedHilGainDecibels = 24.0;
constexpr std::string_view kReportName = "application-flow-report.json";

std::filesystem::path OutputDirectory() {
  // Bazel supplies this before any worker thread starts.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *directory = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  return directory == nullptr || *directory == '\0' ? std::filesystem::current_path()
                                                    : std::filesystem::path(directory);
}

std::filesystem::path RequiredStationPath() {
  const std::optional<std::filesystem::path> path =
      swing_capture::station::StationConfigPathFromEnvironment();
  if (!path.has_value()) {
    throw std::runtime_error(
        "SWING_CAPTURE_STATION_CONFIG is not set; configure it in .bazelrc.local");
  }
  return std::filesystem::absolute(*path);
}

std::filesystem::path HardwareLockPath(const std::filesystem::path &station_path) {
  // Deployment overrides are read before any worker thread starts.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *override_path = std::getenv("SWING_CAPTURE_HARDWARE_LOCK");
  if (override_path != nullptr && *override_path != '\0') {
    return std::filesystem::absolute(override_path);
  }
  return station_path.parent_path() / "artifacts" / "hil" / "hardware.lock";
}

void WriteFile(const std::filesystem::path &path, std::string_view bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("cannot open HIL artifact: " + path.string());
  }
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw std::runtime_error("cannot write HIL artifact: " + path.string());
  }
}

void WriteReport(const std::filesystem::path &output_directory, const Json &report) {
  WriteFile(output_directory / kReportName, report.dump(2) + "\n");
}

Json DeviceInfoJson(const FeatherDeviceInfo &info) {
  return {
      {"firmware", info.firmware},
      {"protocol_version", info.protocol_version},
      {"capabilities", info.capabilities},
      {"tone_lead_min_us", info.tone_minimum_lead_microseconds},
      {"tone_duration_min_us", info.tone_minimum_duration_microseconds},
      {"tone_duration_max_us", info.tone_maximum_duration_microseconds},
      {"tone_frequency_min_hz", info.tone_minimum_frequency_hz},
      {"tone_frequency_max_hz", info.tone_maximum_frequency_hz},
      {"tone_level_min_permille", info.tone_minimum_level_permille},
      {"tone_level_max_permille", info.tone_maximum_level_permille},
      {"swing_step_us", info.swing_step_microseconds},
      {"swing_pre_steps", info.swing_pre_steps},
      {"swing_white_us", info.swing_white_microseconds},
      {"swing_post_steps", info.swing_post_steps},
      {"calibration_candidates", info.calibration_candidates},
      {"fixture_neopixel_gpio", info.fixture_neopixel_gpio},
      {"fixture_neopixel_color_order", info.fixture_neopixel_color_order},
      {"shared_power_gpio", info.shared_power_gpio},
      {"prepare_timeout_us", info.prepare_timeout_microseconds},
      {"wire", info.response.wire_line},
  };
}

class LoopbackServer final {
 public:
  explicit LoopbackServer(swing_capture::service::StationBackend &backend) {
    swing_capture::service::RegisterPreviewRoutes(server_, backend);
    port_ = server_.bind_to_any_port("127.0.0.1");
    if (port_ <= 0) {
      throw std::runtime_error("cannot bind the application HIL loopback server");
    }
    thread_ = std::thread([this] { static_cast<void>(server_.listen_after_bind()); });
    for (int attempt = 0; attempt < 200 && !server_.is_running(); ++attempt) {
      std::this_thread::sleep_for(5ms);
    }
    if (!server_.is_running()) {
      server_.stop();
      thread_.join();
      throw std::runtime_error("application HIL loopback server did not start");
    }
  }

  ~LoopbackServer() {
    server_.stop();
    thread_.join();
  }

  LoopbackServer(const LoopbackServer &) = delete;
  LoopbackServer &operator=(const LoopbackServer &) = delete;
  LoopbackServer(LoopbackServer &&) = delete;
  LoopbackServer &operator=(LoopbackServer &&) = delete;

  [[nodiscard]] int port() const noexcept { return port_; }

 private:
  httplib::Server server_;
  int port_ = 0;
  std::thread thread_;
};

class CaptureDisarmGuard final {
 public:
  explicit CaptureDisarmGuard(httplib::Client &client) : client_(&client) {}
  ~CaptureDisarmGuard() {
    if (armed_) {
      static_cast<void>(
          client_->Post("/api/v1/capture/arm", R"({"armed":false})", "application/json"));
    }
  }

  CaptureDisarmGuard(const CaptureDisarmGuard &) = delete;
  CaptureDisarmGuard &operator=(const CaptureDisarmGuard &) = delete;
  CaptureDisarmGuard(CaptureDisarmGuard &&) = delete;
  CaptureDisarmGuard &operator=(CaptureDisarmGuard &&) = delete;

  void SetArmed(bool armed) noexcept { armed_ = armed; }

 private:
  httplib::Client *client_;
  bool armed_ = false;
};

Json GetJson(httplib::Client &client, std::string_view path) {
  const auto response = client.Get(std::string(path));
  if (!response) {
    throw std::runtime_error("HTTP GET failed: " + std::string(path));
  }
  if (response->status != 200 && response->status != 202) {
    throw std::runtime_error("HTTP GET returned " + std::to_string(response->status) + ": " +
                             std::string(path) + " body=" + response->body);
  }
  return Json::parse(response->body);
}

Json PostJson(httplib::Client &client, std::string_view path, std::string_view body) {
  const auto response = client.Post(std::string(path), std::string(body), "application/json");
  if (!response) {
    throw std::runtime_error("HTTP POST failed: " + std::string(path));
  }
  if (response->status != 200 && response->status != 202) {
    throw std::runtime_error("HTTP POST returned " + std::to_string(response->status) + ": " +
                             std::string(path) + " body=" + response->body);
  }
  return Json::parse(response->body);
}

Json AwaitSyntheticSwing(httplib::Client &client, std::chrono::steady_clock::time_point deadline,
                         Json *statuses) {
  static constexpr std::array<std::string_view, 6> kStages = {"calibrating", "arming",   "stimulus",
                                                              "capturing",   "encoding", "ready"};
  Json status;
  std::size_t last_stage_index = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    status = GetJson(client, "/api/v1/capture/status");
    statuses->push_back(status);
    const Json &hil = status.at("hil");
    if (!status.at("error").get<std::string>().empty() || status.at("state") == "error" ||
        !hil.at("error").get<std::string>().empty() || hil.at("stage") == "error") {
      throw std::runtime_error("synthetic swing application entered error: " + status.dump());
    }
    const std::string stage = hil.at("stage").get<std::string>();
    const auto found = std::ranges::find(kStages, stage);
    if (found == kStages.end()) {
      throw std::runtime_error("synthetic swing reported an unknown stage: " + stage);
    }
    const std::size_t stage_index = static_cast<std::size_t>(std::distance(kStages.begin(), found));
    if (stage_index < last_stage_index) {
      throw std::runtime_error("synthetic swing HIL stage regressed: " + status.dump());
    }
    last_stage_index = stage_index;
    if (stage == "ready" && !hil.at("busy").get<bool>()) {
      return status;
    }
    std::this_thread::sleep_for(20ms);
  }
  throw std::runtime_error("synthetic swing application did not complete; last status=" +
                           status.dump());
}

void RequireConnectedCameras(const Json &status, const StationConfig &station) {
  if (status.at("mode") != "setup_preview" || status.at("cameras").size() != 2U) {
    throw std::runtime_error("dual-camera application status is not nominal");
  }
  const std::array expected = {
      std::pair{"down_the_line", std::string_view(station.down_the_line_camera_serial)},
      std::pair{"face_on", std::string_view(station.face_on_camera_serial)},
  };
  for (const auto &[role, serial] : expected) {
    const auto camera = std::ranges::find_if(status.at("cameras"), [role](const Json &candidate) {
      return candidate.at("role") == role;
    });
    if (camera == status.at("cameras").end() || !camera->at("connected").get<bool>() ||
        camera->at("serial") != serial || !camera->at("error").get<std::string>().empty()) {
      throw std::runtime_error("station role is not connected to its assigned camera");
    }
  }
}

bool HasRenderedCameraPreviews(const Json &status) {
  return std::ranges::all_of(status.at("cameras"), [](const Json &camera) {
    return camera.at("preview_sequence").get<std::uint64_t>() > 0U &&
           camera.at("preview_width").get<std::uint32_t>() > 0U &&
           camera.at("preview_height").get<std::uint32_t>() > 0U &&
           camera.at("image_quality").at("assessment") != "unavailable";
  });
}

void RequireCameraSettingsRestored(const Json &before, const Json &after) {
  for (const Json &initial : before.at("cameras")) {
    const std::string role = initial.at("role").get<std::string>();
    const auto restored = std::ranges::find_if(after.at("cameras"), [&role](const Json &candidate) {
      return candidate.at("role") == role;
    });
    if (restored == after.at("cameras").end() ||
        restored->at("exposure_us").at("value") != initial.at("exposure_us").at("value") ||
        restored->at("gain_db").at("value") != initial.at("gain_db").at("value")) {
      throw std::runtime_error("synthetic HIL did not restore camera settings for " + role);
    }
  }
}

Json AwaitRenderedCameraPreviews(httplib::Client &client, const StationConfig &station,
                                 std::chrono::steady_clock::time_point deadline) {
  Json status;
  while (std::chrono::steady_clock::now() < deadline) {
    status = GetJson(client, "/api/v1/status");
    RequireConnectedCameras(status, station);
    if (HasRenderedCameraPreviews(status)) {
      return status;
    }
    std::this_thread::sleep_for(20ms);
  }
  throw std::runtime_error("both camera previews did not become ready before HIL calibration: " +
                           status.dump());
}

Json RequireAudioTrigger(const Json &status, std::uint64_t baseline_impact_count) {
  const std::uint64_t detected_impacts =
      status.at("audio").at("detected_impacts").get<std::uint64_t>();
  if (status.at("armed").get<bool>() || status.at("audio").at("running").get<bool>() ||
      !status.at("audio").at("ready").get<bool>() || detected_impacts <= baseline_impact_count ||
      detected_impacts - baseline_impact_count != 1U || status.at("last_trigger").is_null()) {
    throw std::runtime_error(
        "one-shot ready session lacks exactly one microphone detection during the stimulus: " +
        status.dump());
  }
  const Json &trigger = status.at("last_trigger");
  const std::int64_t strike = std::stoll(trigger.at("strike_host_monotonic_ns").get<std::string>());
  const std::int64_t confirmation =
      std::stoll(trigger.at("confirmation_host_monotonic_ns").get<std::string>());
  if (trigger.at("source") != "audio" || trigger.at("sample_rate_hz") != 32'000U ||
      trigger.at("peak_amplitude").get<double>() < 0.08 ||
      trigger.at("peak_amplitude").get<double>() <= trigger.at("threshold").get<double>() ||
      trigger.at("noise_floor").get<double>() > trigger.at("threshold").get<double>() ||
      confirmation <= strike) {
    throw std::runtime_error("microphone trigger does not match the Feather tone: " +
                             trigger.dump());
  }
  return trigger;
}

std::uint64_t StimulusBaselineImpactCount(const Json &status_timeline) {
  for (const Json &status : status_timeline) {
    if (status.at("hil").at("stage") == "stimulus") {
      if (status.at("state") != "armed" || !status.at("armed").get<bool>() ||
          !status.at("active_session_id").is_null()) {
        throw std::runtime_error("synthetic stimulus began after capture was already triggered: " +
                                 status.dump());
      }
      return status.at("audio").at("detected_impacts").get<std::uint64_t>();
    }
  }
  throw std::runtime_error("synthetic workflow never exposed its stimulus stage");
}

void FetchPreview(httplib::Client &client, std::string_view role,
                  const std::filesystem::path &output_directory, std::string_view filename) {
  const std::string route = "/api/v1/cameras/" + std::string(role) + "/preview?full=1";
  const auto response = client.Get(route);
  if (!response || response->status != 200 ||
      !response->get_header_value("Content-Type").starts_with("image/png") ||
      !response->body.starts_with(std::string("\x89PNG\r\n\x1a\n", 8))) {
    throw std::runtime_error("full-resolution diagnostic PNG is unavailable for " +
                             std::string(role));
  }
  WriteFile(output_directory / filename, response->body);
}

void CaptureFailureEvidence(httplib::Client &client, const std::filesystem::path &output_directory,
                            Json *report) {
  try {
    (*report)["capture"]["failure_status"] = GetJson(client, "/api/v1/capture/status");
  } catch (const std::exception &error) {
    (*report)["capture"]["failure_status_error"] = error.what();
  }

  Json images = Json::object();
  for (const std::string_view role : {"down_the_line", "face_on"}) {
    const std::string filename = "diagnostic-" + std::string(role) + ".png";
    try {
      FetchPreview(client, role, output_directory, filename);
      images[role] = {{"path", filename}, {"error", ""}};
    } catch (const std::exception &error) {
      images[role] = {{"path", nullptr}, {"error", error.what()}};
    }
  }
  (*report)["failure_diagnostic_images"] = std::move(images);
}

class FailureEvidenceGuard final {
 public:
  FailureEvidenceGuard(httplib::Client &client, const std::filesystem::path &output_directory,
                       Json *report)
      : client_(client), output_directory_(output_directory), report_(report) {}

  ~FailureEvidenceGuard() {
    if (!active_) {
      return;
    }
    try {
      CaptureFailureEvidence(client_, output_directory_, report_);
    } catch (...) {
      // Preserve the primary workflow failure if diagnostic capture itself fails.
    }
  }

  FailureEvidenceGuard(const FailureEvidenceGuard &) = delete;
  FailureEvidenceGuard &operator=(const FailureEvidenceGuard &) = delete;
  FailureEvidenceGuard(FailureEvidenceGuard &&) = delete;
  FailureEvidenceGuard &operator=(FailureEvidenceGuard &&) = delete;

  void Release() noexcept { active_ = false; }

 private:
  httplib::Client &client_;
  const std::filesystem::path &output_directory_;
  Json *report_;
  bool active_ = true;
};

std::string FetchSessionArtifacts(httplib::Client &client, std::string_view session_id,
                                  const std::filesystem::path &directory, Json *evidence) {
  const std::string base = "/api/v1/sessions/" + std::string(session_id) + "/";
  const auto manifest = client.Get(base + "manifest");
  if (!manifest || manifest->status != 200 ||
      !manifest->get_header_value("Content-Type").starts_with("application/json")) {
    throw std::runtime_error("published session manifest is unavailable through HTTP");
  }
  WriteFile(directory / "manifest.json", manifest->body);
  for (const std::string_view role : {"down_the_line", "face_on"}) {
    const std::string media_route = base + std::string(role) + ".webm";
    const auto media = client.Get(media_route);
    if (!media || media->status != 200 ||
        !media->get_header_value("Content-Type").starts_with("video/webm") || media->body.empty()) {
      throw std::runtime_error("published session media is unavailable for " + std::string(role));
    }
    const httplib::Headers range_headers = {{"Range", "bytes=0-15"}};
    const auto range = client.Get(media_route, range_headers);
    if (!range || range->status != 206 || range->body.size() != 16U ||
        range->body != media->body.substr(0, 16) ||
        range->get_header_value("Accept-Ranges") != "bytes" ||
        !range->get_header_value("Content-Range").starts_with("bytes 0-15/")) {
      throw std::runtime_error("published session media does not support browser byte ranges for " +
                               std::string(role));
    }
    (*evidence)[std::string(role)] = {
        {"full_status", media->status},
        {"full_bytes", media->body.size()},
        {"range_status", range->status},
        {"range_bytes", range->body.size()},
        {"accept_ranges", range->get_header_value("Accept-Ranges")},
        {"content_range", range->get_header_value("Content-Range")},
    };
    WriteFile(directory / (std::string(role) + ".webm"), media->body);
  }
  return manifest->body;
}

Json ValidationJson(const SessionArtifactValidation &validation) {
  Json views = Json::array();
  for (const auto &view : validation.views) {
    views.push_back({
        {"role", view.role},
        {"camera_serial", view.camera_serial},
        {"frame_count", view.frame_count},
        {"impact_frame_index", view.impact_frame_index},
        {"nominal_fps", view.nominal_fps},
        {"media_path", view.media_path.string()},
        {"encoded_bytes", view.encoded_bytes},
    });
  }
  Json value = {{"passed", validation.passed}, {"error", validation.error}, {"views", views}};
  Json pipeline_views = Json::array();
  for (const auto &view : validation.pipeline_profile.views) {
    pipeline_views.push_back({
        {"role", view.role},
        {"frame_count", view.frame_count},
        {"timeline_ms", view.timeline_ms},
        {"bayer_fit_demosaic_ms", view.bayer_fit_demosaic_ms},
        {"rgb_to_yuv420_ms", view.rgb_to_yuv420_ms},
        {"codec_encode_ms", view.codec_encode_ms},
        {"webm_mux_ms", view.webm_mux_ms},
        {"finalize_ms", view.finalize_ms},
        {"output_verification_ms", view.output_verification_ms},
        {"total_ms", view.total_ms},
    });
  }
  value["pipeline_profile"] = {
      {"schema_version", validation.pipeline_profile.schema_version},
      {"capture",
       {{"trigger_estimate_to_confirmation_ms",
         validation.pipeline_profile.capture.trigger_estimate_to_confirmation_ms},
        {"confirmation_to_acceptance_ms",
         validation.pipeline_profile.capture.confirmation_to_acceptance_ms},
        {"acceptance_to_freeze_start_ms",
         validation.pipeline_profile.capture.acceptance_to_freeze_start_ms},
        {"freeze_schedule_lateness_ms",
         validation.pipeline_profile.capture.freeze_schedule_lateness_ms},
        {"freeze_and_rotate_ms", validation.pipeline_profile.capture.freeze_and_rotate_ms},
        {"audio_stop_ms", validation.pipeline_profile.capture.audio_stop_ms}}},
      {"session",
       {{"prepublication_analysis_ms",
         validation.pipeline_profile.session.prepublication_analysis_ms},
        {"publisher_planning_ms", validation.pipeline_profile.session.publisher_planning_ms},
        {"validation_and_timeline_ms",
         validation.pipeline_profile.session.validation_and_timeline_ms},
        {"output_setup_ms", validation.pipeline_profile.session.output_setup_ms},
        {"media_encoding_wall_ms", validation.pipeline_profile.session.media_encoding_wall_ms},
        {"frame_metadata_ms", validation.pipeline_profile.session.frame_metadata_ms},
        {"profile_snapshot_after_confirmation_ms",
         validation.pipeline_profile.session.profile_snapshot_after_confirmation_ms},
        {"profile_snapshot_host_monotonic_ns",
         std::to_string(validation.pipeline_profile.session.profile_snapshot_host_monotonic_ns)}}},
      {"views", std::move(pipeline_views)},
  };
  if (validation.synthetic_swing_evidence.has_value()) {
    Json synthetic_views = Json::array();
    for (const auto &view : validation.synthetic_swing_evidence->views) {
      synthetic_views.push_back({
          {"role", view.role},
          {"optical_white_impact_frame_index", view.optical_white_impact_frame_index},
          {"audio_trigger_estimate_offset_us", view.audio_trigger_estimate_offset_microseconds},
          {"mapped_time_correction_us", view.mapped_time_correction_microseconds},
          {"camera_schedule_uncertainty_us", view.camera_schedule_uncertainty_microseconds},
          {"optical_white_passed", view.optical_white_passed},
          {"stable_frame_count", view.stable_frame_count},
          {"matching_frame_count", view.matching_frame_count},
          {"matching_fraction", view.matching_fraction},
          {"mean_signal_delta", view.mean_signal_delta},
          {"mean_expected_color_distance", view.mean_expected_color_distance},
          {"maximum_saturated_fraction", view.maximum_saturated_fraction},
          {"maximum_bloom_fraction", view.maximum_bloom_fraction},
          {"exposure_us", view.exposure_us},
          {"gain_db", view.gain_db},
      });
    }
    value["synthetic_swing_evidence"] = {
        {"selected_brightness", validation.synthetic_swing_evidence->selected_brightness},
        {"views", std::move(synthetic_views)},
    };
  }
  return value;
}

void RequireSyntheticTiming(const SessionArtifactValidation &validation) {
  if (!validation.synthetic_swing_evidence.has_value()) {
    throw std::runtime_error("published session has no synthetic swing timing evidence");
  }
  constexpr std::int64_t kMinimumAudioOffsetUs = 50'000;
  constexpr std::int64_t kMaximumAudioOffsetUs = 250'000;
  constexpr std::int64_t kMaximumCrossCameraOffsetDifferenceUs = 10'000;
  constexpr std::int64_t kMaximumMappedTimeCorrectionUs = 20'000;
  constexpr std::uint64_t kMaximumScheduleUncertaintyUs = 5'000;
  for (const auto &view : validation.synthetic_swing_evidence->views) {
    if (view.audio_trigger_estimate_offset_microseconds < kMinimumAudioOffsetUs ||
        view.audio_trigger_estimate_offset_microseconds > kMaximumAudioOffsetUs ||
        view.mapped_time_correction_microseconds < -kMaximumMappedTimeCorrectionUs ||
        view.mapped_time_correction_microseconds > kMaximumMappedTimeCorrectionUs ||
        view.camera_schedule_uncertainty_microseconds == 0U ||
        view.camera_schedule_uncertainty_microseconds > kMaximumScheduleUncertaintyUs) {
      throw std::runtime_error("synthetic swing camera/audio timing evidence is out of bounds");
    }
  }
  const auto first_offset =
      validation.synthetic_swing_evidence->views[0].audio_trigger_estimate_offset_microseconds;
  const auto second_offset =
      validation.synthetic_swing_evidence->views[1].audio_trigger_estimate_offset_microseconds;
  const std::int64_t difference =
      first_offset > second_offset ? first_offset - second_offset : second_offset - first_offset;
  if (difference > kMaximumCrossCameraOffsetDifferenceUs) {
    throw std::runtime_error("synthetic swing optical/audio offsets disagree across cameras");
  }
}

std::string ReadySessionId(httplib::Client &client, const Json &capture_status) {
  if (!capture_status.at("active_session_id").is_string()) {
    throw std::runtime_error("ready capture does not identify its session");
  }
  const std::string session_id = capture_status.at("active_session_id").get<std::string>();
  const Json sessions = GetJson(client, "/api/v1/sessions");
  const auto found = std::ranges::find_if(
      sessions.at("sessions"),
      [&session_id](const Json &session) { return session.at("session_id") == session_id; });
  if (found == sessions.at("sessions").end() || found->at("state") != "ready" ||
      !found->at("error").get<std::string>().empty()) {
    throw std::runtime_error("ready capture is absent from the published session list");
  }
  return session_id;
}

void RunWorkflow(const std::filesystem::path &output_directory, Json *report) {
  const auto started_at = std::chrono::steady_clock::now();
  const std::filesystem::path station_path = RequiredStationPath();
  const StationConfig station = swing_capture::station::LoadStationConfig(station_path);
  if (!station.camera_roles_verified) {
    throw std::runtime_error("station camera roles must be verified before application HIL");
  }
  (*report)["station"] = {
      {"config_path", station_path.string()},
      {"down_the_line_camera_serial", station.down_the_line_camera_serial},
      {"face_on_camera_serial", station.face_on_camera_serial},
      {"audio_alsa_device", station.audio_alsa_device},
      {"feather_serial_path", station.feather_serial_path.string()},
  };
  (*report)["state"] = "station_configured";
  WriteReport(output_directory, *report);

  const HardwareLock hardware_lock(HardwareLockPath(station_path));
  const std::filesystem::path session_root = output_directory / "station-sessions";
  PreviewStation station_backend(station, session_root, true);
  LoopbackServer server(station_backend);
  httplib::Client client("127.0.0.1", server.port());
  client.set_connection_timeout(kHttpTimeout);
  client.set_read_timeout(kHttpTimeout);
  client.set_write_timeout(kHttpTimeout);
  CaptureDisarmGuard disarm(client);
  FailureEvidenceGuard failure_evidence(client, output_directory, report);

  const Json station_status =
      AwaitRenderedCameraPreviews(client, station, std::chrono::steady_clock::now() + 1s);
  (*report)["camera_status"] = station_status;
  FetchPreview(client, "down_the_line", output_directory, "pre-calibration-down_the_line.png");
  FetchPreview(client, "face_on", output_directory, "pre-calibration-face_on.png");
  (*report)["pre_calibration_images"] = {
      {"down_the_line", "pre-calibration-down_the_line.png"},
      {"face_on", "pre-calibration-face_on.png"},
  };
  {
    FeatherHilSerial serial(station.feather_serial_path);
    FeatherHilController feather(serial);
    const FeatherDeviceInfo feather_info = feather.QueryInfo();
    (*report)["feather"] = {{"info", DeviceInfoJson(feather_info)}};
  }

  const Json preflight = GetJson(client, "/api/v1/capture/status");
  if (preflight.at("schema_version") != 2 || !preflight.at("hil").at("enabled").get<bool>() ||
      preflight.at("hil").at("busy").get<bool>() ||
      !preflight.at("error").get<std::string>().empty() ||
      (preflight.at("state") != "setup" && preflight.at("state") != "ready") ||
      (preflight.at("hil").at("stage") != "idle" && preflight.at("hil").at("stage") != "ready")) {
    throw std::runtime_error("synthetic swing HIL preflight is not ready: " + preflight.dump());
  }
  const std::uint64_t baseline_impact_count =
      preflight.at("audio").at("detected_impacts").get<std::uint64_t>();
  (*report)["capture"]["preflight"] = preflight;

  const auto operation_started_at = std::chrono::steady_clock::now();
  const auto deadline = operation_started_at + kWorkflowDeadline;
  const Json start_response = PostJson(client, "/api/v1/hil/synthetic-swing", "{}");
  disarm.SetArmed(true);
  if (!start_response.at("hil").at("enabled").get<bool>() ||
      !start_response.at("hil").at("busy").get<bool>() ||
      start_response.at("hil").at("stage") != "calibrating") {
    throw std::runtime_error("synthetic swing HIL did not start: " + start_response.dump());
  }
  (*report)["capture"]["start_response"] = start_response;
  (*report)["state"] = "synthetic_swing_started";
  WriteReport(output_directory, *report);

  (*report)["capture"]["status_timeline"] = Json::array();
  const Json ready =
      AwaitSyntheticSwing(client, deadline, &(*report)["capture"]["status_timeline"]);
  (*report)["capture"]["ready"] = ready;
  const std::uint64_t stimulus_baseline_impact_count =
      StimulusBaselineImpactCount((*report)["capture"]["status_timeline"]);
  (*report)["capture"]["stimulus_baseline_impact_count"] = stimulus_baseline_impact_count;
  (*report)["capture"]["preflight_impact_count"] = baseline_impact_count;
  (*report)["capture"]["validated_trigger"] =
      RequireAudioTrigger(ready, stimulus_baseline_impact_count);
  const Json &hil = ready.at("hil");
  if (ready.at("state") != "ready" || ready.at("armed").get<bool>() ||
      hil.at("last_run").is_null() || hil.at("last_run").at("stage") != "ready" ||
      !hil.at("last_run").at("error").get<std::string>().empty() ||
      hil.at("selected_brightness").is_null() ||
      hil.at("last_run").at("selected_brightness") != hil.at("selected_brightness")) {
    throw std::runtime_error("synthetic swing HIL terminal status is incomplete: " + ready.dump());
  }
  const std::uint32_t selected_brightness = hil.at("selected_brightness").get<std::uint32_t>();
  (*report)["selected_brightness"] = selected_brightness;
  const std::string session_id = ReadySessionId(client, ready);
  if (hil.at("last_run").at("session_id") != session_id) {
    throw std::runtime_error("HIL last-run session does not match the capture session");
  }
  (*report)["session_id"] = session_id;

  const Json restored_camera_status = GetJson(client, "/api/v1/status");
  RequireConnectedCameras(restored_camera_status, station);
  RequireCameraSettingsRestored(station_status, restored_camera_status);
  (*report)["restored_camera_status"] = restored_camera_status;

  const std::filesystem::path fetched_session = output_directory / "http-session";
  Json http_evidence;
  const std::string manifest =
      FetchSessionArtifacts(client, session_id, fetched_session, &http_evidence);
  (*report)["session_http"] = std::move(http_evidence);
  const SessionArtifactValidation validation = ValidateSessionArtifacts(
      manifest, fetched_session,
      SessionArtifactExpectations{
          .session_id = session_id,
          .views = {{{.role = "down_the_line",
                      .camera_serial = station.down_the_line_camera_serial,
                      .expected_synthetic_swing_exposure_us = kExpectedHilExposureMicroseconds,
                      .expected_synthetic_swing_gain_db = kExpectedHilGainDecibels},
                     {.role = "face_on",
                      .camera_serial = station.face_on_camera_serial,
                      .expected_synthetic_swing_exposure_us = kExpectedHilExposureMicroseconds,
                      .expected_synthetic_swing_gain_db = kExpectedHilGainDecibels}}},
          .minimum_frame_count = 400,
          .expected_codec = "vp9",
          .require_synthetic_swing_evidence = true,
          .expected_synthetic_swing_brightness = selected_brightness,
      });
  (*report)["session_validation"] = ValidationJson(validation);
  if (!validation.passed) {
    throw std::runtime_error("published session validation failed: " + validation.error);
  }
  RequireSyntheticTiming(validation);

  FetchPreview(client, "down_the_line", output_directory, "diagnostic-down_the_line.png");
  FetchPreview(client, "face_on", output_directory, "diagnostic-face_on.png");
  (*report)["diagnostic_images"] = {
      {"down_the_line", "diagnostic-down_the_line.png"},
      {"face_on", "diagnostic-face_on.png"},
  };
  const Json disarmed = PostJson(client, "/api/v1/capture/arm", R"({"armed":false})");
  disarm.SetArmed(false);
  if (disarmed.at("state") != "setup" || disarmed.at("armed").get<bool>()) {
    throw std::runtime_error("capture application did not disarm cleanly");
  }
  (*report)["capture"]["disarmed"] = disarmed;
  (*report)["elapsed_ms"] =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started_at)
          .count();
  (*report)["operation_elapsed_ms"] = std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - operation_started_at)
                                          .count();
  if (std::chrono::steady_clock::now() > deadline) {
    throw std::runtime_error("application HIL exceeded its 15 second workflow deadline");
  }
  (*report)["passed"] = true;
  (*report)["state"] = "complete";
  failure_evidence.Release();
  WriteReport(output_directory, *report);
}

}  // namespace

int main() {
  const std::filesystem::path output_directory = OutputDirectory();
  Json report = {
      {"schema_version", 2},
      {"test", "synthetic_swing_application_flow"},
      {"passed", false},
      {"state", "starting"},
      {"deadline_ms",
       std::chrono::duration_cast<std::chrono::milliseconds>(kWorkflowDeadline).count()},
  };
  try {
    WriteReport(output_directory, report);
    RunWorkflow(output_directory, &report);
    return 0;
  } catch (const std::exception &error) {
    report["passed"] = false;
    report["state"] = "failed";
    report["error"] = error.what();
    try {
      WriteReport(output_directory, report);
    } catch (...) {
      // Preserve the primary failure when the evidence directory is unavailable.
    }
    return 1;
  }
}
