#include "capture/service/preview_api.h"

#include <httplib.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace swing_capture::service {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

Json NumericSettingJson(const NumericSettingStatus &setting) {
  return {
      {"value", setting.value},
      {"min", setting.minimum},
      {"max", setting.maximum},
      {"increment", setting.increment},
  };
}

Json CameraStatusJson(const CameraStatus &status) {
  return {
      {"role", CameraRoleName(status.role)},
      {"serial", status.serial},
      {"model", status.model},
      {"connected", status.connected},
      {"error", status.error},
      {"stream_fps", status.stream_fps},
      {"preview_sequence", status.preview_sequence},
      {"preview_width", status.preview_width},
      {"preview_height", status.preview_height},
      {"exposure_us", NumericSettingJson(status.exposure_microseconds)},
      {"gain_db", NumericSettingJson(status.gain_decibels)},
      {"image_quality",
       {
           {"assessment", status.image_quality.assessment},
           {"mean", status.image_quality.mean},
           {"p99", status.image_quality.p99},
           {"gradient_energy", status.image_quality.gradient_energy},
       }},
      {"preview_performance",
       {
           {"media_type", status.preview_performance.media_type},
           {"encoded_bytes", status.preview_performance.encoded_bytes},
           {"source_age_ms", status.preview_performance.source_age_milliseconds},
           {"rendered_age_ms", status.preview_performance.rendered_age_milliseconds},
           {"quality_analysis_ms", status.preview_performance.quality_analysis_milliseconds},
           {"bayer_transform_ms", status.preview_performance.bayer_transform_milliseconds},
           {"resize_ms", status.preview_performance.resize_milliseconds},
           {"encode_ms", status.preview_performance.encode_milliseconds},
           {"total_ms", status.preview_performance.total_milliseconds},
       }},
  };
}

Json CaptureTriggerJson(const CaptureTriggerStatus &trigger) {
  return {
      {"source", trigger.source},
      {"strike_host_monotonic_ns", std::to_string(trigger.strike_host_monotonic_nanoseconds)},
      {"confirmation_host_monotonic_ns",
       std::to_string(trigger.confirmation_host_monotonic_nanoseconds)},
      {"sample_rate_hz", trigger.sample_rate_hz},
      {"peak_amplitude", trigger.peak_amplitude},
      {"noise_floor", trigger.noise_floor},
      {"threshold", trigger.threshold},
  };
}

Json HilControlJson(const HilControlStatus &hil) {
  Json last_run = nullptr;
  if (hil.last_run.has_value()) {
    last_run = {
        {"session_id", hil.last_run->session_id.value_or("")},
        {"stage", hil.last_run->stage},
        {"error", hil.last_run->error},
        {"selected_brightness", hil.last_run->selected_brightness.value_or(0)},
    };
    if (!hil.last_run->session_id.has_value()) {
      last_run["session_id"] = nullptr;
    }
    if (!hil.last_run->selected_brightness.has_value()) {
      last_run["selected_brightness"] = nullptr;
    }
  }
  Json value = {
      {"enabled", hil.enabled},
      {"busy", hil.busy},
      {"stage", hil.stage},
      {"error", hil.error},
      {"selected_brightness", hil.selected_brightness.value_or(0)},
      {"last_run", std::move(last_run)},
  };
  if (!hil.selected_brightness.has_value()) {
    value["selected_brightness"] = nullptr;
  }
  return value;
}

Json CaptureStatusJson(const CaptureApplicationStatus &status) {
  Json value = {
      {"schema_version", 2},
      {"state", status.state},
      {"armed", status.armed},
      {"active_session_id", status.active_session_id.value_or("")},
      {"error", status.error},
      {"audio",
       {
           {"running", status.audio_running},
           {"ready", status.audio_ready},
           {"completed_blocks", status.audio_blocks},
           {"completed_samples", status.audio_samples},
           {"detected_impacts", status.detected_impacts},
           {"noise_floor", status.audio_noise_floor},
           {"detection_threshold", status.audio_detection_threshold},
       }},
      {"hil", HilControlJson(status.hil)},
  };
  if (!status.active_session_id.has_value()) {
    value["active_session_id"] = nullptr;
  }
  value["last_trigger"] = status.last_trigger.has_value()
                              ? CaptureTriggerJson(status.last_trigger.value())
                              : Json(nullptr);
  return value;
}

Json SessionSummaryJson(const SessionSummaryStatus &session) {
  return {
      {"session_id", session.session_id},
      {"state", session.state},
      {"created_at_utc", session.created_at_utc},
      {"error", session.error},
  };
}

void SetJson(httplib::Response &response, const Json &value, int status = 200) {
  response.status = status;
  response.set_content(value.dump(), "application/json");
  response.set_header("Cache-Control", "no-store");
}

void SetError(httplib::Response &response, int status, std::string_view message) {
  SetJson(response, {{"error", message}}, status);
}

bool RequireJsonRequest(const httplib::Request &request, httplib::Response &response) {
  if (!request.get_header_value("Content-Type").starts_with("application/json")) {
    SetError(response, 415, "request Content-Type must be application/json");
    return false;
  }
  if (request.has_header("Origin")) {
    const std::string origin = request.get_header_value("Origin");
    const std::size_t authority_start = origin.find("://");
    const std::string_view authority = authority_start == std::string::npos
                                           ? std::string_view{}
                                           : std::string_view(origin).substr(authority_start + 3);
    if (authority.empty() || authority != request.get_header_value("Host")) {
      SetError(response, 403, "cross-origin mutation is not allowed");
      return false;
    }
  }
  return true;
}

CameraSettingsUpdate ParseSettings(std::string_view body) {
  const Json parsed = Json::parse(body);
  if (!parsed.is_object() || parsed.size() != 2 || !parsed.contains("exposure_us") ||
      !parsed.contains("gain_db") || !parsed.at("exposure_us").is_number() ||
      !parsed.at("gain_db").is_number()) {
    throw std::invalid_argument("settings must contain only numeric exposure_us and gain_db");
  }
  const CameraSettingsUpdate update = {
      .exposure_microseconds = parsed.at("exposure_us").get<double>(),
      .gain_decibels = parsed.at("gain_db").get<double>(),
  };
  if (!std::isfinite(update.exposure_microseconds) || !std::isfinite(update.gain_decibels)) {
    throw std::invalid_argument("camera settings must be finite");
  }
  return update;
}

bool ParseArmed(std::string_view body) {
  const Json parsed = Json::parse(body);
  if (!parsed.is_object() || parsed.size() != 1 || !parsed.contains("armed") ||
      !parsed.at("armed").is_boolean()) {
    throw std::invalid_argument("capture arm request must contain only boolean armed");
  }
  return parsed.at("armed").get<bool>();
}

std::optional<CameraRole> RoleFromRequest(const httplib::Request &request) {
  if (request.matches.size() < 2) {
    return std::nullopt;
  }
  return ParseCameraRole(request.matches[1].str());
}

void HandleStatus(StationBackend &backend, httplib::Response &response) {
  try {
    const std::vector<CameraStatus> cameras = backend.CameraStatuses();
    Json camera_values = Json::array();
    bool all_connected = cameras.size() == 2;
    for (const CameraStatus &camera : cameras) {
      camera_values.push_back(CameraStatusJson(camera));
      all_connected = all_connected && camera.connected;
    }
    SetJson(response, {
                          {"schema_version", 1},
                          {"mode", all_connected ? "setup_preview" : "setup_preview_degraded"},
                          {"cameras", std::move(camera_values)},
                      });
  } catch (const std::exception &error) {
    SetError(response, 503, error.what());
  }
}

void HandlePreview(StationBackend &backend, const httplib::Request &request,
                   httplib::Response &response) {
  const std::optional<CameraRole> role = RoleFromRequest(request);
  if (!role.has_value()) {
    SetError(response, 404, "unknown camera role");
    return;
  }
  try {
    const bool full_resolution =
        request.has_param("full") && request.get_param_value("full") == "1";
    const std::optional<PreviewImage> preview = backend.LatestPreview(*role, full_resolution);
    if (!preview.has_value()) {
      SetError(response, 503, "preview is not available yet");
      return;
    }
    response.status = 200;
    response.set_content(preview->bytes, preview->media_type);
    response.set_header("Cache-Control", "no-store");
    response.set_header("X-Preview-Sequence", std::to_string(preview->sequence));
  } catch (const std::invalid_argument &error) {
    SetError(response, 400, error.what());
  } catch (const std::exception &error) {
    SetError(response, 503, error.what());
  }
}

void HandleSettings(StationBackend &backend, const httplib::Request &request,
                    httplib::Response &response) {
  const std::optional<CameraRole> role = RoleFromRequest(request);
  if (!role.has_value()) {
    SetError(response, 404, "unknown camera role");
    return;
  }
  if (!RequireJsonRequest(request, response)) {
    return;
  }
  try {
    const CameraSettingsUpdate settings = ParseSettings(request.body);
    SetJson(response, CameraStatusJson(backend.UpdateCameraSettings(*role, settings)));
  } catch (const Json::exception &error) {
    SetError(response, 400, error.what());
  } catch (const std::invalid_argument &error) {
    SetError(response, 400, error.what());
  } catch (const std::logic_error &error) {
    SetError(response, 409, error.what());
  } catch (const std::exception &error) {
    SetError(response, 503, error.what());
  }
}

void HandleCaptureStatus(StationBackend &backend, httplib::Response &response) {
  try {
    SetJson(response, CaptureStatusJson(backend.CaptureStatus()));
  } catch (const std::exception &error) {
    SetError(response, 503, error.what());
  }
}

void HandleCaptureArm(StationBackend &backend, const httplib::Request &request,
                      httplib::Response &response) {
  if (!RequireJsonRequest(request, response)) {
    return;
  }
  try {
    SetJson(response, CaptureStatusJson(backend.SetCaptureArmed(ParseArmed(request.body))));
  } catch (const Json::exception &error) {
    SetError(response, 400, error.what());
  } catch (const std::invalid_argument &error) {
    SetError(response, 400, error.what());
  } catch (const std::logic_error &error) {
    SetError(response, 409, error.what());
  } catch (const std::exception &error) {
    SetError(response, 503, error.what());
  }
}

void HandleManualCapture(StationBackend &backend, const httplib::Request &request,
                         httplib::Response &response) {
  if (!RequireJsonRequest(request, response)) {
    return;
  }
  try {
    SetJson(response, SessionSummaryJson(backend.CaptureManually()), 202);
  } catch (const std::logic_error &error) {
    SetError(response, 409, error.what());
  } catch (const std::exception &error) {
    SetError(response, 503, error.what());
  }
}

void HandleSyntheticSwingHil(StationBackend &backend, const httplib::Request &request,
                             httplib::Response &response) {
  if (!RequireJsonRequest(request, response)) {
    return;
  }
  try {
    const Json parsed = Json::parse(request.body);
    if (!parsed.is_object() || !parsed.empty()) {
      throw std::invalid_argument("synthetic swing request body must be an empty object");
    }
    SetJson(response, CaptureStatusJson(backend.RunSyntheticSwingHil()), 202);
  } catch (const Json::exception &error) {
    SetError(response, 400, error.what());
  } catch (const std::invalid_argument &error) {
    SetError(response, 400, error.what());
  } catch (const std::logic_error &error) {
    SetError(response, 409, error.what());
  } catch (const std::exception &error) {
    SetError(response, 503, error.what());
  }
}

void HandleSessions(StationBackend &backend, httplib::Response &response) {
  try {
    Json sessions = Json::array();
    for (const SessionSummaryStatus &session : backend.Sessions()) {
      sessions.push_back(SessionSummaryJson(session));
    }
    SetJson(response, {{"schema_version", 1}, {"sessions", std::move(sessions)}});
  } catch (const std::exception &error) {
    SetError(response, 503, error.what());
  }
}

std::string StationEventFingerprint(StationBackend &backend) {
  const CaptureApplicationStatus capture = backend.CaptureStatus();
  Json sessions = Json::array();
  for (const SessionSummaryStatus &session : backend.Sessions()) {
    sessions.push_back({
        {"session_id", session.session_id},
        {"state", session.state},
        {"error", session.error},
    });
  }
  Json impact_previews = nullptr;
  if (capture.active_session_id.has_value()) {
    const auto frame_id = [&backend, &capture](CameraRole role) -> std::optional<std::uint64_t> {
      const auto preview = backend.ImpactPreview(*capture.active_session_id, role);
      return preview.has_value() ? std::optional(preview->frame_id) : std::nullopt;
    };
    impact_previews = {
        {"down_the_line", frame_id(CameraRole::kDownTheLine)},
        {"face_on", frame_id(CameraRole::kFaceOn)},
    };
  }
  return Json({
                  {"capture_state", capture.state},
                  {"armed", capture.armed},
                  {"active_session_id", capture.active_session_id},
                  {"capture_error", capture.error},
                  {"hil_stage", capture.hil.stage},
                  {"hil_busy", capture.hil.busy},
                  {"hil_error", capture.hil.error},
                  {"impact_previews", std::move(impact_previews)},
                  {"sessions", std::move(sessions)},
              })
      .dump();
}

void HandleImpactPreview(StationBackend &backend, const httplib::Request &request,
                         httplib::Response &response) {
  const std::optional<CameraRole> role = ParseCameraRole(request.matches[2].str());
  if (!role.has_value()) {
    SetError(response, 404, "unknown camera role");
    return;
  }
  try {
    const auto preview = backend.ImpactPreview(request.matches[1].str(), *role);
    if (!preview.has_value() || preview->bytes == nullptr) {
      SetError(response, 404, "impact preview is not ready");
      return;
    }
    response.set_content(*preview->bytes, preview->media_type);
    response.set_header("Cache-Control", "no-store");
    response.set_header("X-Swing-Capture-Frame-Id", std::to_string(preview->frame_id));
    response.set_header("X-Swing-Capture-Time-From-Impact-Us",
                        std::to_string(preview->time_from_impact_microseconds));
  } catch (const std::invalid_argument &error) {
    SetError(response, 400, error.what());
  } catch (const std::exception &error) {
    SetError(response, 503, error.what());
  }
}

void HandleStationEvents(StationBackend &backend, httplib::Response &response) {
  constexpr auto kObservationInterval = std::chrono::milliseconds(25);
  constexpr auto kHeartbeatInterval = std::chrono::seconds(15);
  response.set_header("Cache-Control", "no-cache");
  response.set_header("X-Accel-Buffering", "no");
  response.set_chunked_content_provider(
      "text/event-stream",
      [&backend, previous = std::string{}, next_observation = std::chrono::steady_clock::now(),
       next_heartbeat = std::chrono::steady_clock::now(),
       observation_interval = kObservationInterval,
       heartbeat_interval = kHeartbeatInterval](std::size_t, httplib::DataSink &sink) mutable {
        if (!sink.is_writable()) {
          return false;
        }
        std::this_thread::sleep_until(next_observation);
        const auto now = std::chrono::steady_clock::now();
        next_observation = now + observation_interval;
        try {
          std::string current = StationEventFingerprint(backend);
          if (current != previous) {
            previous = std::move(current);
            next_heartbeat = now + heartbeat_interval;
            return sink.write("event: station\ndata: {}\n\n", 25);
          }
        } catch (const std::exception &) {
          // The ordinary JSON status routes retain detailed errors. Wake the
          // client so it can fetch and present them through the same parser.
          return sink.write("event: station\ndata: {}\n\n", 25);
        }
        if (now >= next_heartbeat) {
          next_heartbeat = now + heartbeat_interval;
          return sink.write(": keepalive\n\n", 13);
        }
        return true;
      });
}

void ServeSessionAsset(const std::optional<SessionAsset> &asset, httplib::Response &response) {
  if (!asset.has_value()) {
    SetError(response, 404, "session asset not found");
    return;
  }
  if (!std::filesystem::is_regular_file(asset->path)) {
    SetError(response, 503, "published session asset is unavailable");
    return;
  }
  response.set_file_content(asset->path.string(), asset->media_type);
  const auto served_at = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  response.set_header("X-Swing-Capture-Server-Monotonic-Ns", std::to_string(served_at));
  response.set_header("Accept-Ranges", "bytes");
  response.set_header("Cache-Control", "private, max-age=31536000, immutable");
}

void HandleSessionManifest(StationBackend &backend, const httplib::Request &request,
                           httplib::Response &response) {
  try {
    ServeSessionAsset(backend.SessionManifest(request.matches[1].str()), response);
  } catch (const std::invalid_argument &error) {
    SetError(response, 400, error.what());
  } catch (const std::exception &error) {
    SetError(response, 503, error.what());
  }
}

void HandleSessionMedia(StationBackend &backend, const httplib::Request &request,
                        httplib::Response &response) {
  const std::optional<CameraRole> role = ParseCameraRole(request.matches[2].str());
  if (!role.has_value()) {
    SetError(response, 404, "unknown camera role");
    return;
  }
  try {
    ServeSessionAsset(backend.SessionMedia(request.matches[1].str(), role.value()), response);
  } catch (const std::invalid_argument &error) {
    SetError(response, 400, error.what());
  } catch (const std::exception &error) {
    SetError(response, 503, error.what());
  }
}

void MountStaticFiles(httplib::Server &server, const std::filesystem::path &static_root) {
  if (!std::filesystem::is_directory(static_root)) {
    throw std::invalid_argument("preview static root is not a directory: " + static_root.string());
  }
  if (!server.set_mount_point("/", static_root.string())) {
    throw std::runtime_error("failed to mount preview static root: " + static_root.string());
  }
}

}  // namespace

std::string_view CameraRoleName(CameraRole role) noexcept {
  switch (role) {
    case CameraRole::kDownTheLine:
      return "down_the_line";
    case CameraRole::kFaceOn:
      return "face_on";
  }
  return "unknown";
}

std::optional<CameraRole> ParseCameraRole(std::string_view value) noexcept {
  if (value == "down_the_line") {
    return CameraRole::kDownTheLine;
  }
  if (value == "face_on") {
    return CameraRole::kFaceOn;
  }
  return std::nullopt;
}

void RegisterPreviewRoutes(httplib::Server &server, StationBackend &backend,
                           const std::optional<std::filesystem::path> &static_root) {
  server.set_payload_max_length(4096);
  server.set_default_headers({
      {"Content-Security-Policy",
       "default-src 'self'; img-src 'self' blob:; media-src 'self'; style-src 'self' "
       "'unsafe-inline'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'"},
      {"Referrer-Policy", "no-referrer"},
      {"X-Content-Type-Options", "nosniff"},
      {"X-Frame-Options", "DENY"},
  });
  server.Get("/api/v1/status", [&backend](const httplib::Request &, httplib::Response &response) {
    HandleStatus(backend, response);
  });

  server.Get(R"(/api/v1/cameras/(down_the_line|face_on)/preview\.png)",
             [&backend](const httplib::Request &request, httplib::Response &response) {
               HandlePreview(backend, request, response);
             });
  server.Get(R"(/api/v1/cameras/(down_the_line|face_on)/preview)",
             [&backend](const httplib::Request &request, httplib::Response &response) {
               HandlePreview(backend, request, response);
             });

  server.Patch(R"(/api/v1/cameras/(down_the_line|face_on)/settings)",
               [&backend](const httplib::Request &request, httplib::Response &response) {
                 HandleSettings(backend, request, response);
               });

  server.Get("/api/v1/capture/status",
             [&backend](const httplib::Request &, httplib::Response &response) {
               HandleCaptureStatus(backend, response);
             });
  server.Post("/api/v1/capture/arm",
              [&backend](const httplib::Request &request, httplib::Response &response) {
                HandleCaptureArm(backend, request, response);
              });
  server.Post("/api/v1/capture/manual",
              [&backend](const httplib::Request &request, httplib::Response &response) {
                HandleManualCapture(backend, request, response);
              });
  server.Post("/api/v1/hil/synthetic-swing",
              [&backend](const httplib::Request &request, httplib::Response &response) {
                HandleSyntheticSwingHil(backend, request, response);
              });
  server.Get("/api/v1/sessions", [&backend](const httplib::Request &, httplib::Response &response) {
    HandleSessions(backend, response);
  });
  server.Get("/api/v1/events", [&backend](const httplib::Request &, httplib::Response &response) {
    HandleStationEvents(backend, response);
  });
  server.Get(R"(/api/v1/sessions/([A-Za-z0-9_.-]+)/manifest)",
             [&backend](const httplib::Request &request, httplib::Response &response) {
               HandleSessionManifest(backend, request, response);
             });
  server.Get(R"(/api/v1/sessions/([A-Za-z0-9_.-]+)/(down_the_line|face_on)\.webm)",
             [&backend](const httplib::Request &request, httplib::Response &response) {
               HandleSessionMedia(backend, request, response);
             });
  server.Get(R"(/api/v1/sessions/([A-Za-z0-9_.-]+)/impact/(down_the_line|face_on)\.jpg)",
             [&backend](const httplib::Request &request, httplib::Response &response) {
               HandleImpactPreview(backend, request, response);
             });

  if (static_root.has_value()) {
    MountStaticFiles(server, *static_root);
  }
}

}  // namespace swing_capture::service
