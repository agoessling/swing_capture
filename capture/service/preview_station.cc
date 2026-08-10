#include "capture/service/preview_station.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <ratio>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "capture/application/camera_clip_buffer.h"
#include "capture/application/capture_controller.h"
#include "capture/application/clip_session_publisher.h"
#include "capture/application/session_catalog.h"
#include "capture/audio/audio_capture_session.h"
#include "capture/core/camera_source.h"
#include "capture/daheng/daheng_camera.h"
#include "capture/encoding/clip_session.h"
#include "capture/hil/feather_hil_controller.h"
#include "capture/hil/feather_hil_serial.h"
#include "capture/service/camera_worker.h"
#include "capture/service/preview_api.h"
#include "capture/service/synthetic_swing_hil_operation.h"
#include "capture/service/synthetic_swing_station_workflow.h"
#include "station/station_config.h"

namespace swing_capture::service {
namespace {

using application::CameraCaptureEndpoint;
using application::CameraClipBuffer;
using application::CaptureController;
using application::CaptureControllerStatus;
using application::CapturedSession;
using application::ClipSessionPublisher;
using application::PublishedSessionCatalog;
using application::PublishedSessionRecord;
using application::SessionIdentity;

constexpr std::uint32_t kAudioSampleRateHz = 32000;
constexpr std::size_t kAudioFramesPerBlock = 256;

class DahengPreviewCamera final : public PreviewCameraDevice {
 public:
  DahengPreviewCamera(daheng::GalaxySdk &sdk, const std::string &serial) : camera_(sdk, serial) {}

  [[nodiscard]] CameraIdentity identity() const override { return camera_.identity(); }
  [[nodiscard]] CaptureProfile profile() const override { return camera_.profile(); }
  [[nodiscard]] daheng::DahengDiagnostics diagnostics() const override {
    return camera_.diagnostics();
  }

  void Configure(const daheng::DahengConfiguration &configuration) override {
    camera_.Configure(configuration);
  }
  void Start() override { camera_.Start(); }
  void Stop() override { camera_.Stop(); }
  bool CaptureOne(std::chrono::milliseconds timeout, const daheng::FrameHandler &handler) override {
    return camera_.CaptureOne(timeout, handler);
  }

 private:
  daheng::DahengCamera camera_;
};

CameraStatus DisconnectedStatus(CameraRole role, const std::string &serial,
                                const std::string &startup_error) {
  return {
      .role = role,
      .serial = serial,
      .model = "Assigned Daheng camera",
      .connected = false,
      .error = startup_error,
      .stream_fps = 0.0,
      .preview_sequence = 0,
      .preview_width = 1440,
      .preview_height = 1080,
      .exposure_microseconds = {.value = daheng::kDefaultExposureMicroseconds,
                                .minimum = 1.0,
                                .maximum = 1000000.0,
                                .increment = 1.0},
      .gain_decibels = {.value = daheng::kDefaultGainDecibels,
                        .minimum = 0.0,
                        .maximum = 24.0,
                        .increment = 0.1},
      .image_quality = {.assessment = "unavailable"},
      .preview_performance = {},
  };
}

std::int64_t HostNanoseconds(std::chrono::steady_clock::time_point time) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

CaptureTriggerStatus TriggerStatus(const application::CapturedTrigger &trigger) {
  return {
      .source = std::string(application::CaptureTriggerSourceName(trigger.source)),
      .strike_host_monotonic_nanoseconds = HostNanoseconds(trigger.impact.strike_time),
      .confirmation_host_monotonic_nanoseconds = HostNanoseconds(trigger.impact.confirmation_time),
      .sample_rate_hz = trigger.impact.sample_rate_hz,
      .peak_amplitude = trigger.impact.peak_amplitude,
      .noise_floor = trigger.impact.noise_floor_at_detection,
      .threshold = trigger.impact.threshold_at_detection,
  };
}

CaptureApplicationStatus ApiCaptureStatus(const CaptureControllerStatus &status) {
  const bool audio_failed = !status.audio.error.empty();
  return {
      .state = audio_failed ? "error"
                            : std::string(application::CaptureApplicationStateName(status.state)),
      .armed = status.armed,
      .active_session_id = status.active_session_id,
      .last_trigger = status.last_trigger.has_value()
                          ? std::optional(TriggerStatus(status.last_trigger.value()))
                          : std::nullopt,
      .error = status.error.empty() ? status.audio.error : status.error,
      .audio_running = status.audio.running,
      .audio_ready = status.audio.source_ready,
      .audio_blocks = status.audio.completed_blocks,
      .audio_samples = status.audio.completed_samples,
      .detected_impacts = status.audio.detected_impacts,
      .audio_noise_floor = status.audio.noise_floor,
      .audio_detection_threshold = status.audio.detection_threshold,
      .hil = {},
  };
}

SyntheticSwingCaptureState HilCaptureState(application::CaptureApplicationState state) {
  switch (state) {
    case application::CaptureApplicationState::kSetup:
      return SyntheticSwingCaptureState::kSetup;
    case application::CaptureApplicationState::kArming:
      return SyntheticSwingCaptureState::kArming;
    case application::CaptureApplicationState::kArmed:
      return SyntheticSwingCaptureState::kArmed;
    case application::CaptureApplicationState::kWaitingPostRoll:
      return SyntheticSwingCaptureState::kWaitingPostRoll;
    case application::CaptureApplicationState::kEncoding:
      return SyntheticSwingCaptureState::kEncoding;
    case application::CaptureApplicationState::kReady:
      return SyntheticSwingCaptureState::kReady;
    case application::CaptureApplicationState::kError:
      return SyntheticSwingCaptureState::kError;
  }
  return SyntheticSwingCaptureState::kError;
}

SyntheticSwingCaptureStatus HilCaptureStatus(const CaptureControllerStatus &status) {
  return {
      .state = HilCaptureState(status.state),
      .session_id = status.active_session_id,
      .error = status.error.empty() ? status.audio.error : status.error,
  };
}

std::string_view ApiHilStage(SyntheticSwingHilStage stage) {
  // Arming is part of the calibration/preparation progress step in the web
  // contract; it is retained separately inside the operation for diagnostics.
  return stage == SyntheticSwingHilStage::kArming ? "calibrating"
                                                  : SyntheticSwingHilStageName(stage);
}

std::string IsoUtc(std::chrono::system_clock::time_point now) {
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  if (gmtime_r(&seconds, &utc) == nullptr) {
    throw std::runtime_error("cannot convert session creation time to UTC");
  }
  std::ostringstream formatted;
  formatted << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return formatted.str();
}

}  // namespace

struct PreviewStation::Impl {
  struct CameraSlot {
    CameraRole role;
    std::string serial;
    std::unique_ptr<CameraClipBuffer> clip_buffer;
    std::unique_ptr<CameraWorker> worker;
    std::string startup_error;
  };

  Impl(const station::StationConfig &config, std::filesystem::path output_root,
       bool enable_hil_controls)
      : session_output_root(std::move(output_root)),
        session_catalog(session_output_root),
        feather_serial_path(config.feather_serial_path),
        hil_controls_enabled(enable_hil_controls),
        sdk(std::make_unique<daheng::GalaxySdk>()) {
    if (!config.camera_roles_verified) {
      throw std::invalid_argument("station camera roles must be verified before starting preview");
    }
    if (session_output_root.empty()) {
      throw std::invalid_argument("session output root cannot be empty");
    }
    slots.reserve(2);
    slots.push_back({.role = CameraRole::kDownTheLine,
                     .serial = config.down_the_line_camera_serial,
                     .clip_buffer = std::make_unique<CameraClipBuffer>(),
                     .worker = nullptr,
                     .startup_error = {}});
    slots.push_back({.role = CameraRole::kFaceOn,
                     .serial = config.face_on_camera_serial,
                     .clip_buffer = std::make_unique<CameraClipBuffer>(),
                     .worker = nullptr,
                     .startup_error = {}});
    const std::vector<daheng::DiscoveredCamera> discovered = sdk->Discover(std::chrono::seconds(1));
    for (CameraSlot &slot : slots) {
      const bool present =
          std::ranges::any_of(discovered, [&slot](const daheng::DiscoveredCamera &camera) {
            return camera.identity.serial_number == slot.serial;
          });
      if (!present) {
        slot.startup_error = "assigned camera was not discovered";
        continue;
      }
      try {
        CameraClipBuffer *const clip_buffer = slot.clip_buffer.get();
        slot.worker = std::make_unique<CameraWorker>(
            slot.role, std::make_unique<DahengPreviewCamera>(*sdk, slot.serial),
            daheng::DahengConfiguration{}, nullptr,
            [clip_buffer](const FrameView &frame) { clip_buffer->ObserveFrame(frame); });
        slot.worker->Start();
      } catch (const std::exception &error) {
        slot.startup_error = error.what();
        slot.worker.reset();
      }
    }
    if (std::ranges::all_of(slots, [](const CameraSlot &slot) { return slot.worker != nullptr; })) {
      clip_publisher = std::make_unique<ClipSessionPublisher>(
          application::ClipSessionPublisherConfig{
              .output_root = session_output_root,
              .pre_roll = application::kDefaultCapturePreRoll,
              .post_roll = application::kDefaultCapturePostRoll},
          encoding::MakeVaapiVp9WebmEncoder());
      std::array<CameraCaptureEndpoint, 2> endpoints;
      for (std::size_t index = 0; index < endpoints.size(); ++index) {
        CameraSlot &slot = slots[index];
        endpoints[index] = {
            .role = std::string(CameraRoleName(slot.role)),
            .serial = slot.serial,
            .device_ticks_per_second = slot.worker->TimestampTicksPerSecond(),
            .buffer = slot.clip_buffer.get(),
        };
      }
      controller = std::make_unique<CaptureController>(
          application::CaptureControllerConfig{}, std::move(endpoints),
          MakeArecordAudioCaptureSourceFactory({.capture_executable = "/usr/bin/arecord",
                                                .device = config.audio_alsa_device,
                                                .sample_rate_hz = kAudioSampleRateHz,
                                                .channel_count = config.audio_channel_count,
                                                .selected_channel = config.audio_selected_channel,
                                                .frames_per_block = kAudioFramesPerBlock,
                                                .read_timeout = std::chrono::seconds(2)}),
          kAudioSampleRateHz, [this] { return NextSessionIdentity(); },
          [this](CapturedSession captured) {
            auto hil_evidence = hil_workflow == nullptr
                                    ? std::nullopt
                                    : hil_workflow->AnalyzeCapturedSession(captured);
            application::PublishedSession published =
                clip_publisher->Publish(std::move(captured), std::move(hil_evidence));
            session_catalog.Refresh();
            return published;
          });
      if (hil_controls_enabled) {
        InitializeHilControls();
      }
    }
  }

  SyntheticSwingCameraAnalysisProfile ReadCameraAnalysisProfile(CameraWorker *worker) {
    const std::scoped_lock lock(camera_settings_mutex);
    const CameraStatus status = worker->Status();
    return {
        .exposure_duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::micro>(status.exposure_microseconds.value)),
        .gain_decibels = status.gain_decibels.value,
    };
  }

  void ValidateHilCompletion(const SyntheticSwingCaptureStatus &status) const {
    if (!status.session_id.has_value()) {
      throw std::runtime_error("synthetic swing completed without a published session");
    }
    hil_workflow->ValidatePublishedSession(status.session_id.value());
  }

  void InitializeHilControls() {
    std::array<SyntheticSwingCameraCalibrationSource, 2> calibration_sources;
    for (std::size_t index = 0; index < slots.size(); ++index) {
      const CameraSlot &slot = slots[index];
      CameraWorker *const worker = slot.worker.get();
      const std::string role = std::string(CameraRoleName(slot.role));
      calibration_sources[index] = {
          .role = role,
          .device_ticks_per_second = worker->TimestampTicksPerSecond(),
          .latest_frame = [worker] { return worker->LatestSampledFrame(); },
          .analysis_profile = [this, worker] { return ReadCameraAnalysisProfile(worker); },
          .sampling_interval = [worker] { return worker->LatestFrameSamplingInterval(); },
          .set_sampling_interval =
              [worker](std::chrono::steady_clock::duration interval) {
                worker->SetLatestFrameSamplingInterval(interval);
              },
      };
    }
    hil_workflow = std::make_unique<SyntheticSwingStationWorkflow>(
        std::move(calibration_sources),
        SyntheticSwingStationWorkflowHooks{
            .calibrate_feather = [this] { return CalibrateFeather(); },
            .run_feather_swing =
                [this](std::uint32_t brightness) { return RunFeatherSwing(brightness); },
        });
    hil_operation = std::make_unique<SyntheticSwingHilOperation>(
        SyntheticSwingHilOperationConfig{},
        SyntheticSwingHilOperationHooks{
            .calibrate_brightness =
                [this](const std::stop_token &stop_token) {
                  return hil_workflow->CalibrateBrightness(stop_token);
                },
            .arm_capture = [this] { controller->Arm(); },
            .capture_status = [this] { return HilCaptureStatus(controller->Status()); },
            .run_stimulus =
                [this](std::uint8_t brightness, const std::stop_token &stop_token) {
                  hil_workflow->RunStimulus(brightness, stop_token);
                },
            .validate_completion =
                [this](const SyntheticSwingCaptureStatus &status) {
                  ValidateHilCompletion(status);
                },
            .disarm_capture = [this] { controller->Disarm(); },
        });
  }

  hil::FeatherCalibrationReceipt CalibrateFeather() {
    feather_controller.reset();
    feather_serial = std::make_unique<hil::FeatherHilSerial>(feather_serial_path);
    feather_controller = std::make_unique<hil::FeatherHilController>(*feather_serial);
    static_cast<void>(feather_controller->QueryInfo());
    return feather_controller->CalibrateSwingBrightness();
  }

  hil::FeatherSwingReceipt RunFeatherSwing(std::uint32_t brightness) const {
    if (feather_controller == nullptr) {
      throw std::logic_error("Feather HIL controller was not negotiated during calibration");
    }
    return feather_controller->RunSyntheticSwing(brightness);
  }

  [[nodiscard]] HilControlStatus HilStatus() const {
    HilControlStatus status;
    status.enabled = hil_controls_enabled;
    if (hil_operation == nullptr) {
      return status;
    }
    const SyntheticSwingHilOperationStatus operation = hil_operation->Status();
    status.busy = operation.busy;
    status.stage = ApiHilStage(operation.stage);
    status.error = operation.error;
    status.selected_brightness = operation.selected_brightness;
    if (operation.last_stage.has_value()) {
      status.last_run = HilLastRunStatus{
          .session_id = operation.last_session_id,
          .stage = std::string(ApiHilStage(*operation.last_stage)),
          .error = operation.last_error,
          .selected_brightness = operation.last_selected_brightness,
      };
    }
    return status;
  }

  [[nodiscard]] bool HilBusy() const {
    return hil_operation != nullptr && hil_operation->Status().busy;
  }

  void RequireCamerasHealthyForArming() const {
    for (const CameraSlot &slot : slots) {
      if (slot.worker == nullptr) {
        throw std::logic_error(slot.startup_error);
      }
      const CameraStatus camera = slot.worker->Status();
      if (camera.connected && camera.error.empty()) {
        continue;
      }
      if (controller->Status().armed) {
        controller->Disarm();
      }
      throw std::logic_error(camera.error.empty() ? "camera capture stopped" : camera.error);
    }
  }

  SessionIdentity NextSessionIdentity() {
    const auto now = std::chrono::system_clock::now();
    const auto microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    const std::uint64_t sequence = next_session_sequence.fetch_add(1, std::memory_order_relaxed);
    return {
        .session_id = "swing-" + std::to_string(microseconds) + "-" + std::to_string(sequence),
        .created_at_utc = IsoUtc(now),
    };
  }

  [[nodiscard]] std::optional<PublishedSessionRecord> PublishedSession(
      std::string_view session_id) const {
    return session_catalog.Find(session_id);
  }

  CameraSlot &Slot(CameraRole role) {
    const auto slot = std::ranges::find(slots, role, &CameraSlot::role);
    if (slot == slots.end()) {
      throw std::invalid_argument("unknown camera role");
    }
    return *slot;
  }

  std::filesystem::path session_output_root;
  PublishedSessionCatalog session_catalog;
  std::filesystem::path feather_serial_path;
  bool hil_controls_enabled = false;
  std::unique_ptr<daheng::GalaxySdk> sdk;
  std::vector<CameraSlot> slots;
  std::unique_ptr<ClipSessionPublisher> clip_publisher;
  std::unique_ptr<CaptureController> controller;
  std::unique_ptr<hil::FeatherHilSerial> feather_serial;
  std::unique_ptr<hil::FeatherHilController> feather_controller;
  std::mutex camera_settings_mutex;
  std::unique_ptr<SyntheticSwingStationWorkflow> hil_workflow;
  std::unique_ptr<SyntheticSwingHilOperation> hil_operation;
  std::atomic<std::uint64_t> next_session_sequence = 1;
};

PreviewStation::PreviewStation(const station::StationConfig &config,
                               std::filesystem::path session_output_root, bool enable_hil_controls)
    : impl_(std::make_unique<Impl>(config, std::move(session_output_root), enable_hil_controls)) {}

PreviewStation::~PreviewStation() = default;

std::vector<CameraStatus> PreviewStation::CameraStatuses() {
  std::vector<CameraStatus> statuses;
  statuses.reserve(impl_->slots.size());
  for (Impl::CameraSlot &slot : impl_->slots) {
    statuses.push_back(slot.worker == nullptr
                           ? DisconnectedStatus(slot.role, slot.serial, slot.startup_error)
                           : slot.worker->Status());
  }
  return statuses;
}

std::optional<PreviewImage> PreviewStation::LatestPreview(CameraRole role, bool full_resolution) {
  Impl::CameraSlot &slot = impl_->Slot(role);
  return slot.worker == nullptr ? std::nullopt : slot.worker->LatestPreview(full_resolution);
}

CameraStatus PreviewStation::UpdateCameraSettings(CameraRole role,
                                                  const CameraSettingsUpdate &settings) {
  const std::scoped_lock settings_lock(impl_->camera_settings_mutex);
  if (impl_->HilBusy()) {
    throw std::logic_error("camera settings cannot change during synthetic swing HIL");
  }
  if (impl_->controller != nullptr && impl_->controller->Status().armed) {
    throw std::logic_error("camera settings cannot change while capture is armed");
  }
  Impl::CameraSlot &slot = impl_->Slot(role);
  if (slot.worker == nullptr) {
    throw std::logic_error("camera is disconnected");
  }
  return slot.worker->UpdateSettings(settings);
}

CaptureApplicationStatus PreviewStation::CaptureStatus() {
  if (impl_->controller == nullptr) {
    return {
        .state = "error",
        .armed = false,
        .active_session_id = std::nullopt,
        .last_trigger = std::nullopt,
        .error = "application capture requires both configured cameras",
        .audio_running = false,
        .audio_ready = false,
        .audio_blocks = 0,
        .audio_samples = 0,
        .detected_impacts = 0,
        .audio_noise_floor = 0.0,
        .audio_detection_threshold = 0.0,
        .hil = impl_->HilStatus(),
    };
  }
  CaptureApplicationStatus status = ApiCaptureStatus(impl_->controller->Status());
  status.hil = impl_->HilStatus();
  for (Impl::CameraSlot &slot : impl_->slots) {
    if (slot.worker == nullptr) {
      status.state = "error";
      status.error = slot.startup_error;
      return status;
    }
    const CameraStatus camera = slot.worker->Status();
    if (!camera.connected || !camera.error.empty()) {
      if (status.armed) {
        impl_->controller->Disarm();
        status = ApiCaptureStatus(impl_->controller->Status());
      }
      status.state = "error";
      status.error = camera.error.empty() ? "camera capture stopped" : camera.error;
      return status;
    }
  }
  return status;
}

CaptureApplicationStatus PreviewStation::SetCaptureArmed(bool armed) {
  if (impl_->HilBusy()) {
    throw std::logic_error("normal capture controls are unavailable during synthetic swing HIL");
  }
  if (impl_->controller == nullptr) {
    throw std::logic_error("application capture requires both configured cameras");
  }
  if (armed) {
    impl_->RequireCamerasHealthyForArming();
  }
  const CaptureControllerStatus initial = impl_->controller->Status();
  if ((armed && initial.armed) ||
      (!armed && initial.state == application::CaptureApplicationState::kSetup)) {
    CaptureApplicationStatus status = ApiCaptureStatus(initial);
    status.hil = impl_->HilStatus();
    return status;
  }
  if (armed) {
    impl_->controller->Arm();
  } else {
    impl_->controller->Disarm();
  }
  CaptureApplicationStatus status = ApiCaptureStatus(impl_->controller->Status());
  status.hil = impl_->HilStatus();
  return status;
}

SessionSummaryStatus PreviewStation::CaptureManually() {
  if (impl_->HilBusy()) {
    throw std::logic_error("manual capture is unavailable during synthetic swing HIL");
  }
  if (impl_->controller == nullptr) {
    throw std::logic_error("application capture requires both configured cameras");
  }
  const CaptureApplicationStatus status = CaptureStatus();
  if (status.state == "error") {
    throw std::logic_error(status.error);
  }
  const SessionIdentity identity = impl_->controller->CaptureManually();
  return {
      .session_id = identity.session_id,
      .state = "waiting_post_roll",
      .created_at_utc = identity.created_at_utc,
      .error = {},
  };
}

CaptureApplicationStatus PreviewStation::RunSyntheticSwingHil() {
  const std::scoped_lock settings_lock(impl_->camera_settings_mutex);
  if (!impl_->hil_controls_enabled) {
    throw std::logic_error("synthetic swing HIL controls are disabled");
  }
  if (impl_->hil_operation == nullptr || impl_->hil_workflow == nullptr ||
      impl_->controller == nullptr) {
    throw std::logic_error("synthetic swing HIL requires both configured cameras");
  }
  const CaptureApplicationStatus initial = CaptureStatus();
  if (initial.state == "error") {
    throw std::logic_error(initial.error);
  }
  impl_->hil_operation->Start();
  return CaptureStatus();
}

std::vector<SessionSummaryStatus> PreviewStation::Sessions() {
  std::vector<SessionSummaryStatus> sessions;
  for (const PublishedSessionRecord &session : impl_->session_catalog.Sessions()) {
    sessions.push_back({
        .session_id = session.session_id,
        .state = "ready",
        .created_at_utc = session.created_at_utc,
        .error = {},
    });
  }
  if (impl_->controller != nullptr) {
    const CaptureControllerStatus status = impl_->controller->Status();
    if (status.active_session_identity.has_value()) {
      sessions.insert(
          sessions.begin(),
          {
              .session_id = status.active_session_identity->session_id,
              .state = std::string(application::CaptureApplicationStateName(status.state)),
              .created_at_utc = status.active_session_identity->created_at_utc,
              .error = status.error,
          });
    }
  }
  return sessions;
}

std::optional<SessionAsset> PreviewStation::SessionManifest(std::string_view session_id) {
  const auto session = impl_->PublishedSession(session_id);
  if (!session.has_value()) {
    return std::nullopt;
  }
  return SessionAsset{
      .path = session->manifest_path,
      .media_type = "application/json",
  };
}

std::optional<SessionAsset> PreviewStation::SessionMedia(std::string_view session_id,
                                                         CameraRole role) {
  const auto session = impl_->PublishedSession(session_id);
  if (!session.has_value()) {
    return std::nullopt;
  }
  const std::size_t role_index = role == CameraRole::kDownTheLine ? 0U : 1U;
  return SessionAsset{
      .path = session->media_paths.at(role_index),
      .media_type = "video/webm",
  };
}

}  // namespace swing_capture::service
