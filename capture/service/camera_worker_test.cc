#include "capture/service/camera_worker.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "capture/core/camera_source.h"
#include "capture/daheng/daheng_camera.h"
#include "capture/service/preview_api.h"

namespace {

using swing_capture::CameraIdentity;
using swing_capture::CaptureProfile;
using swing_capture::FrameMetadata;
using swing_capture::FrameView;
using swing_capture::daheng::DahengConfiguration;
using swing_capture::daheng::DahengDiagnostics;
using swing_capture::daheng::FrameHandler;
using swing_capture::daheng::kDefaultExposureMicroseconds;
using swing_capture::daheng::kDefaultGainDecibels;
using swing_capture::preview::PreviewFrameProcessor;
using swing_capture::preview::PreviewRenderOptions;
using swing_capture::preview::RenderedPreviewImage;
using swing_capture::service::CameraRole;
using swing_capture::service::CameraSettingsUpdate;
using swing_capture::service::CameraStatus;
using swing_capture::service::CameraWorker;
using swing_capture::service::PreviewCameraDevice;

class FakeCamera final : public PreviewCameraDevice {
 public:
  CameraIdentity identity() const override {
    return {.model = "Fake MER2", .serial_number = "FAKE123", .vendor = "Test"};
  }

  CaptureProfile profile() const override {
    const std::scoped_lock lock(mutex_);
    return {.width = configuration_.width,
            .height = configuration_.height,
            .frames_per_second = configuration_.target_frames_per_second,
            .pixel_format = "BayerRG8"};
  }

  DahengDiagnostics diagnostics() const override {
    const std::scoped_lock lock(mutex_);
    DahengDiagnostics diagnostics;
    diagnostics.deterministic_free_run_verified = true;
    diagnostics.width = configuration_.width;
    diagnostics.height = configuration_.height;
    diagnostics.requested_exposure_microseconds = configuration_.exposure_microseconds;
    diagnostics.exposure_microseconds = configuration_.exposure_microseconds;
    diagnostics.minimum_exposure_microseconds = 20.0;
    diagnostics.maximum_exposure_microseconds = 10000.0;
    diagnostics.exposure_increment_microseconds = 10.0;
    diagnostics.exposure_increment_valid = true;
    diagnostics.requested_gain_decibels = configuration_.gain_decibels;
    diagnostics.gain_decibels = configuration_.gain_decibels;
    diagnostics.minimum_gain_decibels = 0.0;
    diagnostics.maximum_gain_decibels = 24.0;
    diagnostics.gain_increment_decibels = 0.5;
    diagnostics.gain_increment_valid = true;
    diagnostics.requested_frames_per_second = configuration_.target_frames_per_second;
    diagnostics.target_frames_per_second = configuration_.target_frames_per_second;
    diagnostics.resulting_frames_per_second = configuration_.target_frames_per_second;
    diagnostics.pixel_format = "BayerRG8";
    diagnostics.payload_bytes =
        static_cast<std::uint64_t>(configuration_.width) * configuration_.height;
    return diagnostics;
  }

  void Configure(const DahengConfiguration &configuration) override {
    const std::scoped_lock lock(mutex_);
    events_.push_back("configure:" + std::to_string(configuration.exposure_microseconds) + ":" +
                      std::to_string(configuration.gain_decibels));
    if (fail_next_configuration_) {
      fail_next_configuration_ = false;
      throw std::runtime_error("injected configuration failure");
    }
    configuration_ = configuration;
    pixels_.resize(static_cast<std::size_t>(configuration.width) * configuration.height);
    for (std::size_t index = 0; index < pixels_.size(); ++index) {
      pixels_[index] = static_cast<std::byte>((index * 37U) % 251U);
    }
  }

  void Start() override {
    const std::scoped_lock lock(mutex_);
    assert(!started_);
    events_.push_back("start");
    started_ = true;
  }

  void Stop() override {
    const std::scoped_lock lock(mutex_);
    if (fail_next_stop_) {
      fail_next_stop_ = false;
      throw std::runtime_error("injected stop failure");
    }
    if (started_) {
      events_.push_back("stop");
      started_ = false;
    }
  }

  bool CaptureOne(std::chrono::milliseconds, const FrameHandler &handler) override {
    std::vector<std::byte> pixels;
    DahengConfiguration configuration;
    std::uint64_t frame_id = 0;
    bool return_timeout = false;
    bool return_incomplete = false;
    {
      const std::scoped_lock lock(mutex_);
      assert(started_);
      pixels = pixels_;
      configuration = configuration_;
      frame_id = ++frame_id_;
      return_timeout = return_timeouts_;
      return_incomplete = return_incomplete_frames_;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (return_timeout) {
      return false;
    }
    handler(FrameView{
        .metadata =
            FrameMetadata{
                .frame_id = frame_id,
                .device_timestamp = frame_id * 1000U,
                .host_received_at = std::chrono::steady_clock::now(),
                .width = configuration.width,
                .height = configuration.height,
                .complete = !return_incomplete,
            },
        .payload =
            return_incomplete ? std::span<const std::byte>() : std::span<const std::byte>(pixels),
    });
    return true;
  }

  void FailNextConfiguration() {
    const std::scoped_lock lock(mutex_);
    fail_next_configuration_ = true;
  }

  void FailNextStop() {
    const std::scoped_lock lock(mutex_);
    fail_next_stop_ = true;
  }

  void ReturnTimeouts() {
    const std::scoped_lock lock(mutex_);
    return_timeouts_ = true;
  }

  void ReturnIncompleteFrames() {
    const std::scoped_lock lock(mutex_);
    return_incomplete_frames_ = true;
  }

  std::uint64_t capture_calls() const {
    const std::scoped_lock lock(mutex_);
    return frame_id_;
  }

  std::vector<std::string> events() const {
    const std::scoped_lock lock(mutex_);
    return events_;
  }

 private:
  mutable std::mutex mutex_;
  DahengConfiguration configuration_;
  std::vector<std::byte> pixels_;
  std::vector<std::string> events_;
  std::uint64_t frame_id_ = 0;
  bool started_ = false;
  bool fail_next_configuration_ = false;
  bool fail_next_stop_ = false;
  bool return_timeouts_ = false;
  bool return_incomplete_frames_ = false;
};

class CountingPreviewProcessor final : public PreviewFrameProcessor {
 public:
  RenderedPreviewImage Render(const swing_capture::preview::SampledPreviewFrame &frame,
                              const PreviewRenderOptions &) override {
    render_count_.fetch_add(1U, std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    return {
        .source_metadata = frame.metadata,
        .preview_sequence = frame.preview_sequence,
        .dimensions = {.width = frame.metadata.width, .height = frame.metadata.height},
        .source_quality = {},
        .media_type = "image/jpeg",
        .encoded_bytes = "jpeg",
        .timings = {},
        .render_started_at = now,
        .render_completed_at = now,
    };
  }

  [[nodiscard]] std::uint64_t render_count() const {
    return render_count_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<std::uint64_t> render_count_ = 0;
};

CameraWorker MakeWorker(FakeCamera **fake_out, std::uint32_t width = 8, std::uint32_t height = 6,
                        std::unique_ptr<PreviewFrameProcessor> frame_processor = nullptr) {
  auto fake = std::make_unique<FakeCamera>();
  *fake_out = fake.get();
  return CameraWorker(CameraRole::kDownTheLine, std::move(fake),
                      {.width = width,
                       .height = height,
                       .target_frames_per_second = 200.0,
                       .exposure_microseconds = kDefaultExposureMicroseconds,
                       .gain_decibels = kDefaultGainDecibels,
                       .acquisition_buffer_count = 4},
                      std::move(frame_processor));
}

void WaitForPreview(CameraWorker &worker) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (worker.LatestPreview(false).has_value()) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  assert(false && "preview did not become available");
}

void WaitForDisconnected(CameraWorker &worker) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (!worker.Status().connected) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  assert(false && "camera worker did not report its failure");
}

void TestCapturePreviewAndSettingsLifecycle() {
  FakeCamera *fake = nullptr;
  CameraWorker worker = MakeWorker(&fake, 800, 600);
  worker.Start();
  WaitForPreview(worker);

  const CameraStatus initial = worker.Status();
  assert(initial.connected);
  assert(initial.serial == "FAKE123");
  assert(initial.preview_sequence > 0);
  assert(initial.preview_width == 640);
  assert(initial.preview_height == 480);
  assert(initial.preview_performance.media_type == "image/jpeg");
  assert(initial.preview_performance.encoded_bytes > 0);
  assert(initial.preview_performance.source_age_milliseconds >= 0.0);
  assert(initial.preview_performance.rendered_age_milliseconds >= 0.0);
  assert(initial.preview_performance.total_milliseconds >=
         initial.preview_performance.encode_milliseconds);
  const auto sampled = worker.LatestSampledFrame();
  assert(sampled != nullptr);
  assert(sampled->metadata.width == 800);
  assert(sampled->metadata.height == 600);
  assert(sampled->bayer_pixels.size() == 800U * 600U);
  const auto initial_routine = worker.LatestPreview(false);
  const auto initial_full_resolution = worker.LatestPreview(true);
  assert(initial_routine.has_value());
  assert(initial_full_resolution.has_value());
  assert(initial_full_resolution->sequence >= initial_routine->sequence);
  assert(initial_routine->width == 640);
  assert(initial_routine->height == 480);
  assert(initial_routine->media_type == "image/jpeg");
  assert(initial_full_resolution->width == 800);
  assert(initial_full_resolution->height == 600);
  assert(initial_full_resolution->media_type == "image/png");

  const CameraStatus updated = worker.UpdateSettings(
      CameraSettingsUpdate{.exposure_microseconds = 1770.0, .gain_decibels = 3.5});
  assert(updated.exposure_microseconds.value == 1770.0);
  assert(updated.gain_decibels.value == 3.5);
  assert(updated.preview_sequence == 0);
  assert(worker.LatestSampledFrame() == nullptr);
  assert(!worker.LatestPreview(false).has_value());
  assert(!worker.LatestPreview(true).has_value());

  const std::uint64_t captures_after_update = fake->capture_calls();
  WaitForPreview(worker);
  const auto preview_after_update = worker.LatestPreview(false);
  assert(preview_after_update.has_value());
  assert(preview_after_update->sequence > initial.preview_sequence);
  const auto full_resolution_after_update = worker.LatestPreview(true);
  assert(full_resolution_after_update.has_value());
  assert(full_resolution_after_update->sequence >= preview_after_update->sequence);
  assert(fake->capture_calls() > captures_after_update);

  const std::vector<std::string> events = fake->events();
  const auto stop = std::ranges::find(events, "stop");
  assert(stop != events.end());
  const auto configure = std::find_if(stop, events.end(), [](const std::string &event) {
    return event.starts_with("configure:1770.000000:3.500000");
  });
  assert(configure != events.end());
  assert(std::ranges::find(configure, events.end(), "start") != events.end());

  worker.Stop();
  assert(fake->events().back() == "stop");
  const CameraStatus stopped = worker.Status();
  assert(!stopped.connected);
  assert(stopped.stream_fps == 0.0);
  assert(stopped.error.empty());
  assert(!worker.LatestPreview(false).has_value());
  assert(!worker.LatestPreview(true).has_value());
  assert(worker.LatestSampledFrame() == nullptr);
}

void TestUpdateAfterStopRejectsPromptly() {
  FakeCamera *fake = nullptr;
  CameraWorker worker = MakeWorker(&fake);
  worker.Start();
  worker.Stop();

  const auto started_at = std::chrono::steady_clock::now();
  bool rejected = false;
  try {
    static_cast<void>(worker.UpdateSettings(
        CameraSettingsUpdate{.exposure_microseconds = 1770.0, .gain_decibels = 3.5}));
  } catch (const std::logic_error &) {
    rejected = true;
  }
  const auto elapsed = std::chrono::steady_clock::now() - started_at;
  assert(rejected);
  assert(elapsed < std::chrono::milliseconds(100));
}

void TestRepeatedTimeoutsSurfaceFailureAndClearPreview() {
  FakeCamera *fake = nullptr;
  CameraWorker worker = MakeWorker(&fake);
  worker.Start();
  WaitForPreview(worker);
  fake->ReturnTimeouts();
  WaitForDisconnected(worker);
  const CameraStatus status = worker.Status();
  assert(status.stream_fps == 0.0);
  assert(status.error.find("timed out repeatedly") != std::string::npos);
  assert(!worker.LatestPreview(false).has_value());
  worker.Stop();
}

void TestIncompleteFrameSurfacesFailureAndClearsPreview() {
  FakeCamera *fake = nullptr;
  CameraWorker worker = MakeWorker(&fake);
  worker.Start();
  WaitForPreview(worker);
  fake->ReturnIncompleteFrames();
  WaitForDisconnected(worker);
  const CameraStatus status = worker.Status();
  assert(status.stream_fps == 0.0);
  assert(status.error.find("incomplete frame") != std::string::npos);
  assert(!worker.LatestPreview(false).has_value());
  worker.Stop();
}

void TestStopFailureDuringSettingsUpdateIsFatal() {
  FakeCamera *fake = nullptr;
  CameraWorker worker = MakeWorker(&fake);
  worker.Start();
  fake->FailNextStop();

  bool failed = false;
  try {
    static_cast<void>(worker.UpdateSettings(
        CameraSettingsUpdate{.exposure_microseconds = 1770.0, .gain_decibels = 3.5}));
  } catch (const std::runtime_error &) {
    failed = true;
  }
  assert(failed);
  WaitForDisconnected(worker);
  const std::uint64_t captures_after_failure = fake->capture_calls();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  assert(fake->capture_calls() == captures_after_failure);
  const CameraStatus status = worker.Status();
  assert(status.stream_fps == 0.0);
  assert(status.error.find("injected stop failure") != std::string::npos);
  assert(!worker.LatestPreview(false).has_value());
  worker.Stop();
}

void TestRejectedSettingDoesNotStopCamera() {
  FakeCamera *fake = nullptr;
  CameraWorker worker = MakeWorker(&fake);
  worker.Start();
  const std::size_t events_before = fake->events().size();
  bool rejected = false;
  try {
    static_cast<void>(worker.UpdateSettings(
        CameraSettingsUpdate{.exposure_microseconds = 100000.0, .gain_decibels = 0.0}));
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  assert(rejected);
  assert(fake->events().size() == events_before);
  worker.Stop();
}

void TestFailedUpdateRestoresPreviousConfiguration() {
  FakeCamera *fake = nullptr;
  CameraWorker worker = MakeWorker(&fake);
  worker.Start();
  fake->FailNextConfiguration();
  bool failed = false;
  try {
    static_cast<void>(worker.UpdateSettings(
        CameraSettingsUpdate{.exposure_microseconds = 1770.0, .gain_decibels = 3.5}));
  } catch (const std::runtime_error &) {
    failed = true;
  }
  assert(failed);
  const CameraStatus recovered = worker.Status();
  assert(recovered.connected);
  assert(recovered.exposure_microseconds.value == kDefaultExposureMicroseconds);
  assert(recovered.gain_decibels.value == kDefaultGainDecibels);
  worker.Stop();
}

void TestHighRateSamplingDoesNotIncreaseRoutineRenderCadence() {
  using namespace std::chrono_literals;
  FakeCamera *fake = nullptr;
  auto processor = std::make_unique<CountingPreviewProcessor>();
  CountingPreviewProcessor *const processor_observer = processor.get();
  CameraWorker worker = MakeWorker(&fake, 8, 6, std::move(processor));
  assert(worker.LatestFrameSamplingInterval() == 33ms);
  worker.SetLatestFrameSamplingInterval(8ms);
  assert(worker.LatestFrameSamplingInterval() == 8ms);
  worker.Start();

  const std::uint64_t first_capture = fake->capture_calls();
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (fake->capture_calls() < first_capture + 100U &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(2ms);
  }
  assert(fake->capture_calls() >= first_capture + 100U);
  const auto sampled = worker.LatestSampledFrame();
  assert(sampled != nullptr);
  assert(sampled->preview_sequence >= 20U);
  const std::uint64_t render_count = processor_observer->render_count();
  assert(render_count > 0U);
  assert(sampled->preview_sequence > render_count * 2U);
  worker.Stop();
}

}  // namespace

int main() {
  const auto run = [](std::string_view name, auto test) {
    std::cerr << "RUN " << name << '\n';
    test();
  };
  run("capture/settings lifecycle", TestCapturePreviewAndSettingsLifecycle);
  run("update after stop", TestUpdateAfterStopRejectsPromptly);
  run("repeated timeouts", TestRepeatedTimeoutsSurfaceFailureAndClearPreview);
  run("incomplete frame", TestIncompleteFrameSurfacesFailureAndClearsPreview);
  run("stop failure", TestStopFailureDuringSettingsUpdateIsFatal);
  run("rejected setting", TestRejectedSettingDoesNotStopCamera);
  run("failed update rollback", TestFailedUpdateRestoresPreviousConfiguration);
  run("high-rate sample render throttle", TestHighRateSamplingDoesNotIncreaseRoutineRenderCadence);
  return 0;
}
