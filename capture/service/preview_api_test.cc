#include "capture/service/preview_api.h"

#include <httplib.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

using swing_capture::service::CameraRole;
using swing_capture::service::CameraSettingsUpdate;
using swing_capture::service::CameraStatus;
using swing_capture::service::CaptureApplicationStatus;
using swing_capture::service::CaptureTriggerStatus;
using swing_capture::service::HilLastRunStatus;
using swing_capture::service::NumericSettingStatus;
using swing_capture::service::PreviewImage;
using swing_capture::service::PreviewPerformanceStatus;
using swing_capture::service::PreviewQualityStatus;
using swing_capture::service::SessionAsset;
using swing_capture::service::SessionSummaryStatus;
using swing_capture::service::StationBackend;

CameraStatus FakeStatus(CameraRole role, std::string serial) {
  return {
      .role = role,
      .serial = std::move(serial),
      .model = "MER2-160-227U3C",
      .connected = true,
      .error = "",
      .stream_fps = 226.8,
      .preview_sequence = 42,
      .preview_width = 1440,
      .preview_height = 1080,
      .exposure_microseconds =
          NumericSettingStatus{
              .value = 4000.0, .minimum = 20.0, .maximum = 4400.0, .increment = 1.0},
      .gain_decibels =
          NumericSettingStatus{.value = 0.0, .minimum = 0.0, .maximum = 24.0, .increment = 0.1},
      .image_quality =
          PreviewQualityStatus{
              .assessment = "nominal", .mean = 112.0, .p99 = 238.0, .gradient_energy = 44.0},
      .preview_performance =
          PreviewPerformanceStatus{
              .media_type = "image/jpeg",
              .encoded_bytes = 40000,
              .source_age_milliseconds = 18.0,
              .rendered_age_milliseconds = 12.0,
              .quality_analysis_milliseconds = 1.0,
              .bayer_transform_milliseconds = 3.0,
              .resize_milliseconds = 0.0,
              .encode_milliseconds = 1.0,
              .total_milliseconds = 5.0,
          },
  };
}

class FakeBackend final : public StationBackend {
 public:
  FakeBackend()
      : cameras_({FakeStatus(CameraRole::kDownTheLine, "DOWN123"),
                  FakeStatus(CameraRole::kFaceOn, "FACE456")}) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char *temporary = std::getenv("TEST_TMPDIR");
    assert(temporary != nullptr);
    session_root_ = std::filesystem::path(temporary) / "api-session";
    std::filesystem::create_directories(session_root_);
    std::ofstream(session_root_ / "manifest.json") << R"({"schema_version":1})";
    std::ofstream(session_root_ / "down_the_line.webm", std::ios::binary) << "down-media";
    std::ofstream(session_root_ / "face_on.webm", std::ios::binary) << "face-media";
  }

  std::vector<CameraStatus> CameraStatuses() override { return cameras_; }

  std::optional<PreviewImage> LatestPreview(CameraRole role, bool full_resolution) override {
    last_full_resolution_ = full_resolution;
    if (!preview_available_) {
      return std::nullopt;
    }
    return PreviewImage{
        .sequence = role == CameraRole::kDownTheLine ? 42U : 43U,
        .width = full_resolution ? 4U : 2U,
        .height = full_resolution ? 2U : 1U,
        .media_type = "image/png",
        .bytes = std::string("\x89PNG\r\n\x1a\n", 8),
    };
  }

  CameraStatus UpdateCameraSettings(CameraRole role,
                                    const CameraSettingsUpdate &settings) override {
    CameraStatus &camera = cameras_[role == CameraRole::kDownTheLine ? 0 : 1];
    camera.exposure_microseconds.value = settings.exposure_microseconds;
    camera.gain_decibels.value = settings.gain_decibels;
    return camera;
  }

  CaptureApplicationStatus CaptureStatus() override { return capture_status_; }

  CaptureApplicationStatus SetCaptureArmed(bool armed) override {
    capture_status_.armed = armed;
    capture_status_.state = armed ? "armed" : "setup";
    return capture_status_;
  }

  SessionSummaryStatus CaptureManually() override {
    capture_status_.active_session_id = "session-1";
    return {.session_id = "session-1",
            .state = "encoding",
            .created_at_utc = "2026-08-09T00:00:00Z",
            .error = {}};
  }

  CaptureApplicationStatus RunSyntheticSwingHil() override {
    if (!capture_status_.hil.enabled) {
      throw std::logic_error("synthetic swing HIL controls are disabled");
    }
    capture_status_.hil.busy = true;
    capture_status_.hil.stage = "calibrating";
    return capture_status_;
  }

  std::vector<SessionSummaryStatus> Sessions() override {
    return {{.session_id = "session-1",
             .state = "ready",
             .created_at_utc = "2026-08-09T00:00:00Z",
             .error = {}}};
  }

  std::optional<SessionAsset> SessionManifest(std::string_view session_id) override {
    return session_id == "session-1"
               ? std::optional(SessionAsset{.path = session_root_ / "manifest.json",
                                            .media_type = "application/json"})
               : std::nullopt;
  }

  std::optional<SessionAsset> SessionMedia(std::string_view session_id, CameraRole role) override {
    if (session_id != "session-1") {
      return std::nullopt;
    }
    return SessionAsset{
        .path =
            session_root_ / (std::string(swing_capture::service::CameraRoleName(role)) + ".webm"),
        .media_type = "video/webm"};
  }

  bool preview_available_ = true;
  bool last_full_resolution_ = false;

 private:
  std::vector<CameraStatus> cameras_;
  std::filesystem::path session_root_;
  CaptureApplicationStatus capture_status_ = {
      .state = "setup",
      .armed = false,
      .active_session_id = std::nullopt,
      .last_trigger = CaptureTriggerStatus{.source = "audio",
                                           .strike_host_monotonic_nanoseconds = 100,
                                           .confirmation_host_monotonic_nanoseconds = 120,
                                           .sample_rate_hz = 32000,
                                           .peak_amplitude = 0.5,
                                           .noise_floor = 0.01,
                                           .threshold = 0.08},
      .error = {},
      .audio_running = false,
      .audio_ready = false,
      .audio_blocks = 0,
      .audio_samples = 0,
      .detected_impacts = 1,
      .audio_noise_floor = 0.01,
      .audio_detection_threshold = 0.08,
      .hil = {.enabled = true,
              .busy = false,
              .stage = "idle",
              .error = {},
              .selected_brightness = std::nullopt,
              .last_run = HilLastRunStatus{.session_id = "session-previous",
                                           .stage = "ready",
                                           .error = {},
                                           .selected_brightness = 24}},
  };
};

class TestServer final {
 public:
  TestServer(FakeBackend &backend,
             const std::optional<std::filesystem::path> &static_root = std::nullopt) {
    swing_capture::service::RegisterPreviewRoutes(server_, backend, static_root);
    port_ = server_.bind_to_any_port("127.0.0.1");
    assert(port_ > 0);
    thread_ = std::thread([this] { assert(server_.listen_after_bind()); });
    for (int attempt = 0; attempt < 100 && !server_.is_running(); ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(server_.is_running());
  }

  ~TestServer() {
    server_.stop();
    thread_.join();
  }

  [[nodiscard]] int port() const noexcept { return port_; }

 private:
  httplib::Server server_;
  int port_ = 0;
  std::thread thread_;
};

void TestStatusAndPreviewRoutes() {
  FakeBackend backend;
  TestServer server(backend);
  httplib::Client client("127.0.0.1", server.port());

  const auto status = client.Get("/api/v1/status");
  assert(status);
  assert(status->status == 200);
  assert(status->get_header_value("Content-Type").starts_with("application/json"));
  const nlohmann::json parsed = nlohmann::json::parse(status->body);
  assert(parsed.at("schema_version") == 1);
  assert(parsed.at("mode") == "setup_preview");
  assert(parsed.at("cameras").size() == 2);
  assert(parsed.at("cameras").at(0).at("role") == "down_the_line");
  assert(parsed.at("cameras").at(0).at("error") == "");
  assert(parsed.at("cameras").at(0).at("preview_performance").at("media_type") == "image/jpeg");
  assert(parsed.at("cameras").at(0).at("preview_performance").at("total_ms") == 5.0);

  const auto png = client.Get("/api/v1/cameras/down_the_line/preview?sequence=41");
  assert(png);
  assert(png->status == 200);
  assert(png->get_header_value("Content-Type").starts_with("image/png"));
  assert(png->get_header_value("Cache-Control") == "no-store");
  assert(png->get_header_value("X-Preview-Sequence") == "42");
  assert(png->body == std::string("\x89PNG\r\n\x1a\n", 8));

  const auto full = client.Get("/api/v1/cameras/down_the_line/preview?sequence=42&full=1");
  assert(full);
  assert(full->status == 200);
  assert(full->get_header_value("X-Preview-Sequence") == "42");
  assert(backend.last_full_resolution_);

  backend.preview_available_ = false;
  const auto unavailable = client.Get("/api/v1/cameras/face_on/preview");
  assert(unavailable);
  assert(unavailable->status == 503);
}

void TestSettingsRouteValidationAndReadback() {
  FakeBackend backend;
  TestServer server(backend);
  httplib::Client client("127.0.0.1", server.port());

  const auto updated = client.Patch("/api/v1/cameras/face_on/settings",
                                    R"({"exposure_us":1770,"gain_db":3.5})", "application/json");
  assert(updated);
  assert(updated->status == 200);
  const nlohmann::json parsed = nlohmann::json::parse(updated->body);
  assert(parsed.at("role") == "face_on");
  assert(parsed.at("exposure_us").at("value") == 1770.0);
  assert(parsed.at("gain_db").at("value") == 3.5);

  const auto malformed = client.Patch("/api/v1/cameras/face_on/settings", "{}", "application/json");
  assert(malformed);
  assert(malformed->status == 400);

  const auto extra =
      client.Patch("/api/v1/cameras/face_on/settings",
                   R"({"exposure_us":1770,"gain_db":3.5,"frame_rate":1})", "application/json");
  assert(extra);
  assert(extra->status == 400);

  const auto wrong_type = client.Patch("/api/v1/cameras/face_on/settings",
                                       R"({"exposure_us":1770,"gain_db":3.5})", "text/plain");
  assert(wrong_type && wrong_type->status == 415);
}

void TestCaptureSessionRoutes() {
  FakeBackend backend;
  TestServer server(backend);
  httplib::Client client("127.0.0.1", server.port());

  const auto initial = client.Get("/api/v1/capture/status");
  assert(initial && initial->status == 200);
  Json status = Json::parse(initial->body);
  assert(status.at("schema_version") == 2);
  assert(status.at("state") == "setup");
  assert(status.at("last_trigger").at("source") == "audio");
  assert(status.at("last_trigger").at("peak_amplitude") == 0.5);
  assert(status.at("audio").at("noise_floor") == 0.01);
  assert(status.at("audio").at("detection_threshold") == 0.08);
  assert(status.at("hil").at("enabled") == true);
  assert(status.at("hil").at("selected_brightness").is_null());
  assert(status.at("hil").at("last_run").at("session_id") == "session-previous");
  assert(status.at("hil").at("last_run").at("selected_brightness") == 24);

  const auto armed = client.Post("/api/v1/capture/arm", R"({"armed":true})", "application/json");
  assert(armed && armed->status == 200);
  status = Json::parse(armed->body);
  assert(status.at("state") == "armed");
  assert(status.at("armed") == true);
  const auto malformed = client.Post("/api/v1/capture/arm", "{}", "application/json");
  assert(malformed && malformed->status == 400);

  const httplib::Headers foreign_origin = {{"Origin", "http://attacker.test"}};
  const auto cross_origin =
      client.Post("/api/v1/capture/arm", foreign_origin, R"({"armed":false})", "application/json");
  assert(cross_origin && cross_origin->status == 403);

  const auto manual = client.Post("/api/v1/capture/manual", "", "application/json");
  assert(manual && manual->status == 202);
  assert(Json::parse(manual->body).at("session_id") == "session-1");

  const auto hil = client.Post("/api/v1/hil/synthetic-swing", "{}", "application/json");
  assert(hil && hil->status == 202);
  assert(Json::parse(hil->body).at("hil").at("stage") == "calibrating");
  const auto malformed_hil =
      client.Post("/api/v1/hil/synthetic-swing", R"({"brightness":255})", "application/json");
  assert(malformed_hil && malformed_hil->status == 400);

  const auto sessions = client.Get("/api/v1/sessions");
  assert(sessions && sessions->status == 200);
  assert(Json::parse(sessions->body).at("sessions").at(0).at("state") == "ready");

  const auto manifest = client.Get("/api/v1/sessions/session-1/manifest");
  assert(manifest && manifest->status == 200);
  assert(manifest->get_header_value("Content-Type").starts_with("application/json"));
  assert(std::stoll(manifest->get_header_value("X-Swing-Capture-Server-Monotonic-Ns")) > 0);
  assert(Json::parse(manifest->body).at("schema_version") == 1);
  const auto media = client.Get("/api/v1/sessions/session-1/down_the_line.webm");
  assert(media && media->status == 200);
  assert(media->get_header_value("Content-Type").starts_with("video/webm"));
  assert(media->body == "down-media");
  const httplib::Headers range_headers = {{"Range", "bytes=2-5"}};
  const auto media_range =
      client.Get("/api/v1/sessions/session-1/down_the_line.webm", range_headers);
  assert(media_range);
  assert(media_range->status == 206);
  assert(media_range->body == "wn-m");
  assert(media_range->get_header_value("Accept-Ranges") == "bytes");
  assert(media_range->get_header_value("Content-Range") == "bytes 2-5/10");
  assert(std::stoll(media_range->get_header_value("X-Swing-Capture-Server-Monotonic-Ns")) > 0);
  assert(media_range->get_header_value("X-Content-Type-Options") == "nosniff");
  const auto missing = client.Get("/api/v1/sessions/missing/manifest");
  assert(missing && missing->status == 404);
}

void TestStaticAssets() {
  const char *temporary = std::getenv("TEST_TMPDIR");
  assert(temporary != nullptr);
  const std::filesystem::path root = std::filesystem::path(temporary) / "static";
  std::filesystem::create_directory(root);
  {
    std::ofstream index(root / "index.html");
    index << "<!doctype html><title>preview</title>";
    assert(index);
  }

  FakeBackend backend;
  TestServer server(backend, root);
  httplib::Client client("127.0.0.1", server.port());
  const auto index = client.Get("/");
  assert(index);
  assert(index->status == 200);
  assert(index->body.find("preview") != std::string::npos);
  assert(index->get_header_value("Content-Type").starts_with("text/html"));
  assert(index->get_header_value("X-Frame-Options") == "DENY");
}

}  // namespace

int main() {
  TestStatusAndPreviewRoutes();
  TestSettingsRouteValidationAndReadback();
  TestCaptureSessionRoutes();
  TestStaticAssets();
  return 0;
}
