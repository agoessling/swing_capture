#include "capture/service/preview_api.h"

#include <httplib.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using swing_capture::service::CameraRole;
using swing_capture::service::CameraSettingsUpdate;
using swing_capture::service::CameraStatus;
using swing_capture::service::NumericSettingStatus;
using swing_capture::service::PreviewPng;
using swing_capture::service::PreviewQualityStatus;
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
  };
}

class FakeBackend final : public StationBackend {
 public:
  FakeBackend()
      : cameras_({FakeStatus(CameraRole::kDownTheLine, "DOWN123"),
                  FakeStatus(CameraRole::kFaceOn, "FACE456")}) {}

  std::vector<CameraStatus> CameraStatuses() override { return cameras_; }

  std::optional<PreviewPng> LatestPreview(CameraRole role, bool full_resolution) override {
    last_full_resolution_ = full_resolution;
    if (!preview_available_) {
      return std::nullopt;
    }
    return PreviewPng{
        .sequence = role == CameraRole::kDownTheLine ? 42U : 43U,
        .width = full_resolution ? 4U : 2U,
        .height = full_resolution ? 2U : 1U,
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

  bool preview_available_ = true;
  bool last_full_resolution_ = false;

 private:
  std::vector<CameraStatus> cameras_;
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

void TestStatusAndPngRoutes() {
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

  const auto png = client.Get("/api/v1/cameras/down_the_line/preview.png?sequence=41");
  assert(png);
  assert(png->status == 200);
  assert(png->get_header_value("Content-Type").starts_with("image/png"));
  assert(png->get_header_value("Cache-Control") == "no-store");
  assert(png->get_header_value("X-Preview-Sequence") == "42");
  assert(png->body == std::string("\x89PNG\r\n\x1a\n", 8));

  const auto full = client.Get("/api/v1/cameras/down_the_line/preview.png?sequence=42&full=1");
  assert(full);
  assert(full->status == 200);
  assert(full->get_header_value("X-Preview-Sequence") == "42");
  assert(backend.last_full_resolution_);

  backend.preview_available_ = false;
  const auto unavailable = client.Get("/api/v1/cameras/face_on/preview.png");
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
}

}  // namespace

int main() {
  TestStatusAndPngRoutes();
  TestSettingsRouteValidationAndReadback();
  TestStaticAssets();
  return 0;
}
