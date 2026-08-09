#include "capture/service/preview_api.h"

#include <httplib.h>

#include <cmath>
#include <exception>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

void SetJson(httplib::Response &response, const Json &value, int status = 200) {
  response.status = status;
  response.set_content(value.dump(), "application/json");
  response.set_header("Cache-Control", "no-store");
}

void SetError(httplib::Response &response, int status, std::string_view message) {
  SetJson(response, {{"error", message}}, status);
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

  if (static_root.has_value()) {
    MountStaticFiles(server, *static_root);
  }
}

}  // namespace swing_capture::service
