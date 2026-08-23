#include "capture/service/camera_worker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <ratio>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include "capture/core/camera_source.h"
#include "capture/daheng/daheng_camera.h"
#include "capture/image/image_quality.h"
#include "capture/preview/camera_settings.h"
#include "capture/preview/latest_frame_sampler.h"
#include "capture/preview/preview_image.h"
#include "capture/service/preview_api.h"

namespace swing_capture::service {
namespace {

constexpr std::chrono::milliseconds kCaptureTimeout(50);
constexpr std::chrono::milliseconds kPreviewInterval(33);
constexpr std::chrono::seconds kSettingsWaitTimeout(5);
constexpr std::chrono::seconds kFullResolutionWaitTimeout(3);
constexpr std::size_t kMaximumConsecutiveTimeouts = 3;
constexpr std::size_t kMaximumBayerPayload = static_cast<std::size_t>(1440U) * 1080U;
constexpr std::uint32_t kMaximumPreviewWidth = 640;
constexpr std::uint32_t kMaximumPreviewHeight = 480;

double PositiveIncrement(double increment, bool valid) {
  return valid && std::isfinite(increment) && increment > 0.0 ? increment : 1.0;
}

double AlignMaximum(double minimum, double maximum, double increment) {
  if (maximum <= minimum) {
    return minimum;
  }
  const double steps = std::floor((maximum - minimum) / increment);
  return minimum + steps * increment;
}

NumericSettingStatus ExposureStatus(const daheng::DahengDiagnostics &diagnostics) {
  const double increment = PositiveIncrement(diagnostics.exposure_increment_microseconds,
                                             diagnostics.exposure_increment_valid);
  double maximum = diagnostics.maximum_exposure_microseconds;
  if (diagnostics.target_frames_per_second > 0.0) {
    maximum = std::min(maximum, 1000000.0 / diagnostics.target_frames_per_second);
  }
  maximum = AlignMaximum(diagnostics.minimum_exposure_microseconds, maximum, increment);
  return {
      .value = diagnostics.exposure_microseconds,
      .minimum = diagnostics.minimum_exposure_microseconds,
      .maximum = std::max(maximum, diagnostics.exposure_microseconds),
      .increment = increment,
  };
}

NumericSettingStatus GainStatus(const daheng::DahengDiagnostics &diagnostics) {
  return {
      .value = diagnostics.gain_decibels,
      .minimum = diagnostics.minimum_gain_decibels,
      .maximum = diagnostics.maximum_gain_decibels,
      .increment =
          PositiveIncrement(diagnostics.gain_increment_decibels, diagnostics.gain_increment_valid),
  };
}

PreviewQualityStatus QualityStatus(const image::ImageQualityMetrics &metrics) {
  std::string assessment = "nominal";
  if (metrics.mean < 20.0 || metrics.p99 < 64U) {
    assessment = "too_dark";
  } else if (metrics.near_white_fraction > 0.20 || (metrics.mean > 225.0 && metrics.p99 >= 250U)) {
    assessment = "too_bright";
  } else if (metrics.gradient_energy < 16.0) {
    assessment = "soft";
  }
  return {
      .assessment = std::move(assessment),
      .mean = metrics.mean,
      .p99 = static_cast<double>(metrics.p99),
      .gradient_energy = metrics.gradient_energy,
  };
}

preview::CameraSettingRanges ValidationRanges(const daheng::DahengDiagnostics &diagnostics) {
  const NumericSettingStatus exposure = ExposureStatus(diagnostics);
  const NumericSettingStatus gain = GainStatus(diagnostics);
  return {
      .exposure_microseconds = {.minimum = exposure.minimum,
                                .maximum = exposure.maximum,
                                .increment = exposure.increment},
      .gain_db = {.minimum = gain.minimum, .maximum = gain.maximum, .increment = gain.increment},
  };
}

std::string ValidationErrorName(preview::SettingValidationError error) {
  switch (error) {
    case preview::SettingValidationError::kNone:
      return "none";
    case preview::SettingValidationError::kInvalidRange:
      return "camera reported an invalid range";
    case preview::SettingValidationError::kNotFinite:
      return "value is not finite";
    case preview::SettingValidationError::kBelowMinimum:
      return "value is below the minimum";
    case preview::SettingValidationError::kAboveMaximum:
      return "value is above the maximum";
    case preview::SettingValidationError::kNotAlignedToIncrement:
      return "value is not aligned to the camera increment";
  }
  return "unknown validation failure";
}

std::string PublishErrorName(preview::PreviewFramePublishResult result) {
  switch (result) {
    case preview::PreviewFramePublishResult::kPublished:
      [[fallthrough]];
    case preview::PreviewFramePublishResult::kRateLimited:
      return "none";
    case preview::PreviewFramePublishResult::kIncompleteFrame:
      return "camera delivered an incomplete frame";
    case preview::PreviewFramePublishResult::kInvalidDimensions:
      return "camera delivered invalid frame dimensions";
    case preview::PreviewFramePublishResult::kPayloadSizeMismatch:
      return "camera frame payload does not match its dimensions";
    case preview::PreviewFramePublishResult::kPayloadTooLarge:
      return "camera frame payload exceeds the preview capacity";
  }
  return "camera delivered an invalid preview frame";
}

double Milliseconds(std::chrono::steady_clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

}  // namespace

struct CameraWorker::Impl {
  enum class RendererStage {
    kIdle,
    kRoutine,
    kFullResolution,
  };

  struct SettingsCommand {
    CameraSettingsUpdate settings;
    std::promise<CameraStatus> completion;
  };

  Impl(CameraRole camera_role, std::unique_ptr<PreviewCameraDevice> camera_device,
       daheng::DahengConfiguration initial_configuration,
       std::unique_ptr<preview::PreviewFrameProcessor> preview_frame_processor,
       CapturedFrameSink frame_sink, PreviewSamplingHook sampling_hook)
      : role(camera_role),
        camera(std::move(camera_device)),
        configuration(initial_configuration),
        frame_processor(preview_frame_processor == nullptr ? preview::MakeSoftwarePreviewProcessor()
                                                           : std::move(preview_frame_processor)),
        captured_frame_sink(std::move(frame_sink)),
        preview_sampling_hook(std::move(sampling_hook)),
        sampler(
            {.minimum_interval = kPreviewInterval, .maximum_payload_bytes = kMaximumBayerPayload}) {
    if (camera == nullptr) {
      throw std::invalid_argument("camera worker requires a camera device");
    }
    identity = camera->identity();
  }

  void Start() {
    if (worker.joinable() || renderer.joinable()) {
      throw std::logic_error("camera worker is already started");
    }
    PrepareForStart();
    renderer = std::jthread([this](const std::stop_token &stop_token) { RenderLoop(stop_token); });
    auto startup = std::make_shared<std::promise<void>>();
    std::future<void> ready = startup->get_future();
    try {
      worker = std::jthread(
          [this, startup](const std::stop_token &stop_token) { Run(stop_token, startup); });
      ready.get();
    } catch (...) {
      CloseCommandAcceptance(
          std::make_exception_ptr(std::runtime_error("camera worker failed during startup")));
      if (worker.joinable()) {
        worker.request_stop();
        worker.join();
      }
      renderer.request_stop();
      render_condition.notify_all();
      renderer.join();
      throw;
    }
  }

  void Stop() noexcept {
    CloseCommandAcceptance(
        std::make_exception_ptr(std::runtime_error("camera worker is stopping")));
    if (worker.joinable()) {
      worker.request_stop();
      worker.join();
    }
    if (renderer.joinable()) {
      renderer.request_stop();
      render_condition.notify_all();
      renderer.join();
    }
    RecordStopped();
  }

  void Run(const std::stop_token &stop_token,
           const std::shared_ptr<std::promise<void>> &startup) noexcept {
    bool startup_reported = false;
    bool streaming = false;
    try {
      camera->Configure(configuration);
      camera->Start();
      streaming = true;
      {
        const std::scoped_lock lock(state_mutex);
        diagnostics = camera->diagnostics();
        connected = true;
        failure.clear();
        rate_started_at = std::chrono::steady_clock::now();
      }
      OpenCommandAcceptance();
      startup->set_value();
      startup_reported = true;

      std::size_t consecutive_timeouts = 0;
      while (!stop_token.stop_requested()) {
        if (renderer_failed.load(std::memory_order_acquire)) {
          throw std::runtime_error("preview renderer failed");
        }
        streaming = ProcessSettingsCommands(streaming);
        const bool delivered = camera->CaptureOne(
            kCaptureTimeout, [this](const FrameView &frame) { PublishFrame(frame); });
        if (!delivered) {
          ++consecutive_timeouts;
          if (consecutive_timeouts >= kMaximumConsecutiveTimeouts) {
            throw std::runtime_error("camera timed out repeatedly while capturing preview frames");
          }
          continue;
        }
        consecutive_timeouts = 0;
      }
      camera->Stop();
      streaming = false;
      CloseCommandAcceptance(std::make_exception_ptr(std::runtime_error("camera worker stopped")));
      RecordStopped();
    } catch (...) {
      const std::exception_ptr error = std::current_exception();
      CloseCommandAcceptance(error);
      if (!startup_reported) {
        startup->set_exception(error);
      }
      RecordFailureAndInvalidate(error);
    }

    if (streaming) {
      try {
        camera->Stop();
      } catch (...) {
        RecordFailureAndInvalidate(std::current_exception());
      }
    }
  }

  bool ProcessSettingsCommands(bool streaming) {
    std::shared_ptr<SettingsCommand> command;
    {
      const std::scoped_lock lock(command_mutex);
      if (commands.empty()) {
        return streaming;
      }
      command = std::move(commands.front());
      commands.pop_front();
    }

    try {
      ApplySettings(command->settings, &streaming);
      command->completion.set_value(Status());
    } catch (...) {
      command->completion.set_exception(std::current_exception());
      if (!streaming) {
        {
          const std::scoped_lock lock(command_mutex);
          settings_pending = false;
        }
        throw;
      }
    }
    {
      const std::scoped_lock lock(command_mutex);
      settings_pending = false;
    }
    return streaming;
  }

  void ApplySettings(const CameraSettingsUpdate &settings, bool *streaming) {
    daheng::DahengDiagnostics current_diagnostics;
    {
      const std::scoped_lock lock(state_mutex);
      current_diagnostics = diagnostics;
    }
    const preview::CameraSettingsValidation validation =
        preview::ValidateCameraSettings({.exposure_microseconds = settings.exposure_microseconds,
                                         .gain_db = settings.gain_decibels},
                                        ValidationRanges(current_diagnostics));
    if (!validation.exposure_microseconds.valid()) {
      throw std::invalid_argument("invalid exposure_us: " +
                                  ValidationErrorName(validation.exposure_microseconds.error));
    }
    if (!validation.gain_db.valid()) {
      throw std::invalid_argument("invalid gain_db: " +
                                  ValidationErrorName(validation.gain_db.error));
    }

    const daheng::DahengConfiguration previous = configuration;
    daheng::DahengConfiguration requested = configuration;
    requested.exposure_microseconds = settings.exposure_microseconds;
    requested.gain_decibels = settings.gain_decibels;
    try {
      camera->Stop();
      *streaming = false;
    } catch (...) {
      // The stream state is unknown after GXStreamOff fails. Treat the worker
      // as failed instead of attempting another dequeue or configuration.
      *streaming = false;
      throw;
    }
    try {
      camera->Configure(requested);
      InvalidatePreview();
      camera->Start();
      *streaming = true;
      configuration = requested;
      const std::scoped_lock lock(state_mutex);
      diagnostics = camera->diagnostics();
    } catch (...) {
      const std::exception_ptr update_error = std::current_exception();
      try {
        camera->Configure(previous);
        camera->Start();
        *streaming = true;
        configuration = previous;
        const std::scoped_lock lock(state_mutex);
        diagnostics = camera->diagnostics();
      } catch (...) {
        std::throw_with_nested(
            std::runtime_error("camera settings update failed and the previous configuration "
                               "could not be restored"));
      }
      std::rethrow_exception(update_error);
    }
  }

  void PublishFrame(const FrameView &frame) {
    const auto callback_received_at = std::chrono::steady_clock::now();
    {
      // Record the SDK callback boundary before any host-side capture-ring or
      // preview work. Otherwise a blocked sink or sampler looks like a camera
      // acquisition stall even though the frame was already dequeued.
      const std::scoped_lock lock(state_mutex);
      latest_capture_frame_id = frame.metadata.frame_id;
      latest_capture_received_at = callback_received_at;
    }
    if (captured_frame_sink) {
      captured_frame_sink(frame);
    }
    {
      const std::scoped_lock lock(state_mutex);
      latest_sink_frame_id = frame.metadata.frame_id;
      latest_sink_completed_at = std::chrono::steady_clock::now();
    }
    if (preview_sampling_hook) {
      preview_sampling_hook();
    }
    const preview::PreviewFramePublishResult result = sampler.TryPublish(frame);
    if (result != preview::PreviewFramePublishResult::kPublished &&
        result != preview::PreviewFramePublishResult::kRateLimited) {
      throw std::runtime_error(PublishErrorName(result));
    }
    if (result == preview::PreviewFramePublishResult::kPublished) {
      RequestRoutineRender(frame.metadata.host_received_at);
    }

    const std::scoped_lock lock(state_mutex);
    latest_sampler_frame_id = frame.metadata.frame_id;
    latest_sampler_completed_at = std::chrono::steady_clock::now();
    ++frames_in_rate_window;
    const auto elapsed = frame.metadata.host_received_at - rate_started_at;
    if (elapsed >= std::chrono::seconds(1)) {
      observed_frames_per_second = static_cast<double>(frames_in_rate_window) /
                                   std::chrono::duration<double>(elapsed).count();
      frames_in_rate_window = 0;
      rate_started_at = frame.metadata.host_received_at;
    }
  }

  void RequestRoutineRender(std::chrono::steady_clock::time_point frame_time) {
    bool notify = false;
    {
      const std::scoped_lock lock(render_wait_mutex);
      if (!has_requested_routine_render ||
          frame_time - last_routine_render_requested_at >= kPreviewInterval) {
        render_requested = true;
        has_requested_routine_render = true;
        last_routine_render_requested_at = frame_time;
        notify = true;
      }
    }
    if (notify) {
      render_condition.notify_one();
    }
  }

  void RequestFullResolutionRender() {
    {
      const std::scoped_lock lock(render_wait_mutex);
      full_resolution_render_requested = true;
    }
    render_condition.notify_one();
  }

  void RenderLoop(const std::stop_token &stop_token) noexcept {
    for (;;) {
      bool render_routine = false;
      bool render_full_resolution = false;
      {
        std::unique_lock lock(render_wait_mutex);
        render_condition.wait(lock, stop_token, [this] {
          return render_requested || full_resolution_render_requested;
        });
        if (stop_token.stop_requested()) {
          return;
        }
        render_routine = std::exchange(render_requested, false);
        render_full_resolution = std::exchange(full_resolution_render_requested, false);
        renderer_stage = render_routine ? RendererStage::kRoutine : RendererStage::kFullResolution;
      }

      try {
        if (render_routine) {
          RenderLatestRoutine();
        }
        if (render_full_resolution) {
          {
            const std::scoped_lock lock(render_wait_mutex);
            renderer_stage = RendererStage::kFullResolution;
          }
          RenderLatestFullResolution();
        }
        {
          const std::scoped_lock lock(render_wait_mutex);
          renderer_stage = RendererStage::kIdle;
        }
      } catch (...) {
        {
          const std::scoped_lock lock(render_wait_mutex);
          renderer_stage = RendererStage::kIdle;
        }
        const std::exception_ptr error = std::current_exception();
        renderer_failed.store(true, std::memory_order_release);
        CloseCommandAcceptance(error);
        RecordFailureAndInvalidate(error);
        return;
      }
    }
  }

  void RenderLatestRoutine() {
    const std::shared_ptr<const preview::SampledPreviewFrame> latest = sampler.Latest();
    if (latest == nullptr) {
      return;
    }
    std::uint64_t generation = 0;
    {
      const std::scoped_lock lock(render_mutex);
      if (rendered_routine != nullptr &&
          rendered_routine->preview_sequence >= latest->preview_sequence) {
        return;
      }
      generation = preview_generation;
    }

    auto next = std::make_shared<preview::RenderedPreviewImage>(
        frame_processor->Render(*latest, {.maximum_width = kMaximumPreviewWidth,
                                          .maximum_height = kMaximumPreviewHeight,
                                          .image_format = preview::PreviewImageFormat::kJpeg,
                                          .jpeg_quality = 85,
                                          .quality_options = {}}));
    const std::scoped_lock lock(render_mutex);
    if (generation == preview_generation &&
        (rendered_routine == nullptr ||
         rendered_routine->preview_sequence < next->preview_sequence)) {
      rendered_routine = std::move(next);
    }
  }

  void RenderLatestFullResolution() {
    const std::shared_ptr<const preview::SampledPreviewFrame> latest = sampler.Latest();
    if (latest == nullptr) {
      full_resolution_condition.notify_all();
      return;
    }
    std::uint64_t generation = 0;
    {
      const std::scoped_lock lock(render_mutex);
      if (rendered_full_resolution != nullptr &&
          rendered_full_resolution->preview_sequence >= latest->preview_sequence) {
        full_resolution_condition.notify_all();
        return;
      }
      generation = preview_generation;
    }

    auto next = std::make_shared<preview::RenderedPreviewImage>(
        frame_processor->Render(*latest, {.maximum_width = latest->metadata.width,
                                          .maximum_height = latest->metadata.height,
                                          .image_format = preview::PreviewImageFormat::kPng,
                                          .quality_options = {}}));
    {
      const std::scoped_lock lock(render_mutex);
      if (generation == preview_generation &&
          (rendered_full_resolution == nullptr ||
           rendered_full_resolution->preview_sequence < next->preview_sequence)) {
        rendered_full_resolution = std::move(next);
      }
    }
    full_resolution_condition.notify_all();
  }

  CameraStatus Status() {
    daheng::DahengDiagnostics current_diagnostics;
    bool current_connected = false;
    double current_fps = 0.0;
    std::string current_failure;
    {
      const std::scoped_lock lock(state_mutex);
      current_diagnostics = diagnostics;
      current_connected = connected;
      current_fps = observed_frames_per_second;
      current_failure = failure;
    }
    CameraStatus status = {
        .role = role,
        .serial = identity.serial_number,
        .model = identity.model,
        .connected = current_connected,
        .error = std::move(current_failure),
        .stream_fps = current_fps,
        .preview_sequence = 0,
        .preview_width = 0,
        .preview_height = 0,
        .exposure_microseconds = ExposureStatus(current_diagnostics),
        .gain_decibels = GainStatus(current_diagnostics),
        .image_quality = {},
        .preview_performance = {},
    };
    std::shared_ptr<const preview::RenderedPreviewImage> current_preview;
    {
      const std::scoped_lock lock(render_mutex);
      current_preview = rendered_routine;
    }
    status.preview_performance = BuildPreviewPerformance(current_preview);
    if (current_preview != nullptr) {
      status.preview_sequence = current_preview->preview_sequence;
      status.preview_width = current_preview->dimensions.width;
      status.preview_height = current_preview->dimensions.height;
      status.image_quality = QualityStatus(current_preview->source_quality);
    }
    return status;
  }

  PreviewPerformanceStatus BuildPreviewPerformance(
      const std::shared_ptr<const preview::RenderedPreviewImage> &current_preview) {
    const auto now = std::chrono::steady_clock::now();
    std::uint64_t current_capture_frame_id = 0;
    std::chrono::steady_clock::time_point current_capture_received_at;
    std::uint64_t current_sink_frame_id = 0;
    std::chrono::steady_clock::time_point current_sink_completed_at;
    std::uint64_t current_sampler_frame_id = 0;
    std::chrono::steady_clock::time_point current_sampler_completed_at;
    {
      const std::scoped_lock lock(state_mutex);
      current_capture_frame_id = latest_capture_frame_id;
      current_capture_received_at = latest_capture_received_at;
      current_sink_frame_id = latest_sink_frame_id;
      current_sink_completed_at = latest_sink_completed_at;
      current_sampler_frame_id = latest_sampler_frame_id;
      current_sampler_completed_at = latest_sampler_completed_at;
    }
    const std::shared_ptr<const preview::SampledPreviewFrame> current_sampled = sampler.Latest();
    RendererStage current_renderer_stage = RendererStage::kIdle;
    bool current_render_pending = false;
    {
      const std::scoped_lock lock(render_wait_mutex);
      current_renderer_stage = renderer_stage;
      current_render_pending = render_requested || full_resolution_render_requested;
    }
    const auto renderer_stage_name = [current_renderer_stage] {
      switch (current_renderer_stage) {
        case RendererStage::kIdle:
          return "idle";
        case RendererStage::kRoutine:
          return "routine";
        case RendererStage::kFullResolution:
          return "full_resolution";
      }
      return "unknown";
    };

    PreviewPerformanceStatus performance;
    performance.latest_capture_frame_id = current_capture_frame_id;
    performance.latest_capture_age_milliseconds =
        current_capture_frame_id == 0
            ? 0.0
            : std::max(0.0, Milliseconds(now - current_capture_received_at));
    performance.latest_sink_frame_id = current_sink_frame_id;
    performance.latest_sink_completion_age_milliseconds =
        current_sink_frame_id == 0 ? 0.0
                                   : std::max(0.0, Milliseconds(now - current_sink_completed_at));
    performance.latest_sampler_frame_id = current_sampler_frame_id;
    performance.latest_sampler_completion_age_milliseconds =
        current_sampler_frame_id == 0
            ? 0.0
            : std::max(0.0, Milliseconds(now - current_sampler_completed_at));
    performance.sampled_sequence =
        current_sampled == nullptr ? 0 : current_sampled->preview_sequence;
    performance.sampled_age_milliseconds =
        current_sampled == nullptr
            ? 0.0
            : std::max(0.0, Milliseconds(now - current_sampled->metadata.host_received_at));
    performance.renderer_stage = renderer_stage_name();
    performance.render_pending = current_render_pending;
    if (current_preview != nullptr) {
      performance.media_type = current_preview->media_type;
      performance.encoded_bytes = current_preview->encoded_bytes.size();
      performance.source_age_milliseconds =
          std::max(0.0, Milliseconds(now - current_preview->source_metadata.host_received_at));
      performance.rendered_age_milliseconds =
          std::max(0.0, Milliseconds(now - current_preview->render_completed_at));
      performance.quality_analysis_milliseconds =
          Milliseconds(current_preview->timings.quality_analysis);
      performance.bayer_transform_milliseconds =
          Milliseconds(current_preview->timings.bayer_transform);
      performance.resize_milliseconds = Milliseconds(current_preview->timings.resize);
      performance.encode_milliseconds = Milliseconds(current_preview->timings.encode);
      performance.total_milliseconds = Milliseconds(current_preview->timings.total);
      performance.render_queue_milliseconds =
          std::max(0.0, Milliseconds(current_preview->render_started_at -
                                     current_preview->source_metadata.host_received_at));
    }
    return performance;
  }

  std::uint64_t TimestampTicksPerSecond() {
    const std::scoped_lock lock(state_mutex);
    return diagnostics.timestamp_ticks_per_second;
  }

  std::optional<PreviewImage> LatestPreview(bool full_resolution) {
    std::shared_ptr<const preview::RenderedPreviewImage> current;
    if (!full_resolution) {
      const std::scoped_lock lock(render_mutex);
      current = rendered_routine;
    } else {
      std::uint64_t minimum_sequence = 0;
      std::uint64_t generation = 0;
      {
        const std::scoped_lock lock(render_mutex);
        if (rendered_routine == nullptr) {
          return std::nullopt;
        }
        minimum_sequence = rendered_routine->preview_sequence;
        generation = preview_generation;
        if (rendered_full_resolution != nullptr &&
            rendered_full_resolution->preview_sequence >= minimum_sequence) {
          current = rendered_full_resolution;
        }
      }
      if (current == nullptr) {
        RequestFullResolutionRender();
        std::unique_lock lock(render_mutex);
        const bool ready = full_resolution_condition.wait_for(
            lock, kFullResolutionWaitTimeout, [this, generation, minimum_sequence] {
              return preview_generation != generation ||
                     (rendered_full_resolution != nullptr &&
                      rendered_full_resolution->preview_sequence >= minimum_sequence) ||
                     renderer_failed.load(std::memory_order_acquire);
            });
        if (!ready || preview_generation != generation || rendered_full_resolution == nullptr ||
            rendered_full_resolution->preview_sequence < minimum_sequence) {
          return std::nullopt;
        }
        current = rendered_full_resolution;
      }
    }
    if (current == nullptr) {
      return std::nullopt;
    }
    return PreviewImage{
        .sequence = current->preview_sequence,
        .width = current->dimensions.width,
        .height = current->dimensions.height,
        .media_type = current->media_type,
        .bytes = current->encoded_bytes,
        .performance = BuildPreviewPerformance(current),
    };
  }

  CameraStatus UpdateSettings(const CameraSettingsUpdate &settings) {
    auto command = std::make_shared<SettingsCommand>();
    command->settings = settings;
    std::future<CameraStatus> result = command->completion.get_future();
    {
      const std::scoped_lock command_lock(command_mutex);
      if (!accepting_commands) {
        throw std::logic_error("camera worker is not accepting settings updates");
      }
      if (settings_pending) {
        throw std::logic_error("a camera settings update is already pending");
      }
      settings_pending = true;
      commands.push_back(std::move(command));
    }
    if (result.wait_for(kSettingsWaitTimeout) != std::future_status::ready) {
      throw std::runtime_error("camera settings update timed out");
    }
    return result.get();
  }

  void PrepareForStart() {
    InvalidatePreview();
    renderer_failed.store(false, std::memory_order_release);
    {
      const std::scoped_lock lock(state_mutex);
      connected = false;
      failure.clear();
      rate_started_at = {};
      frames_in_rate_window = 0;
      observed_frames_per_second = 0.0;
      latest_capture_frame_id = 0;
      latest_capture_received_at = {};
      latest_sink_frame_id = 0;
      latest_sink_completed_at = {};
      latest_sampler_frame_id = 0;
      latest_sampler_completed_at = {};
    }
    const std::scoped_lock lock(command_mutex);
    accepting_commands = false;
    settings_pending = false;
    commands.clear();
  }

  void OpenCommandAcceptance() {
    const std::scoped_lock lock(command_mutex);
    accepting_commands = true;
  }

  void CloseCommandAcceptance(const std::exception_ptr &error) noexcept {
    std::deque<std::shared_ptr<SettingsCommand>> pending;
    {
      const std::scoped_lock lock(command_mutex);
      accepting_commands = false;
      pending = std::move(commands);
      if (!pending.empty()) {
        settings_pending = false;
      }
    }
    for (const auto &command : pending) {
      try {
        command->completion.set_exception(error);
      } catch (const std::future_error &) {
        // The drain has sole ownership of queued commands, so an already
        // completed or unbound promise indicates an internal invariant breach.
        std::terminate();
      }
    }
  }

  void InvalidatePreview() {
    {
      const std::scoped_lock lock(render_mutex);
      sampler.Reset();
      rendered_routine.reset();
      rendered_full_resolution.reset();
      ++preview_generation;
    }
    full_resolution_condition.notify_all();
    {
      const std::scoped_lock lock(render_wait_mutex);
      render_requested = false;
      full_resolution_render_requested = false;
      has_requested_routine_render = false;
      last_routine_render_requested_at = {};
    }
  }

  void RecordStopped() noexcept {
    InvalidatePreview();
    const std::scoped_lock lock(state_mutex);
    connected = false;
    frames_in_rate_window = 0;
    observed_frames_per_second = 0.0;
  }

  void RecordFailureAndInvalidate(const std::exception_ptr &error) noexcept {
    std::string message = "unknown camera failure";
    try {
      if (error != nullptr) {
        std::rethrow_exception(error);
      }
    } catch (const std::exception &caught) {
      message = caught.what();
    } catch (...) {
      message = "non-standard camera failure";
    }
    InvalidatePreview();
    const std::scoped_lock lock(state_mutex);
    connected = false;
    observed_frames_per_second = 0.0;
    failure = std::move(message);
  }

  CameraRole role;
  std::unique_ptr<PreviewCameraDevice> camera;
  CameraIdentity identity;
  daheng::DahengConfiguration configuration;
  std::unique_ptr<preview::PreviewFrameProcessor> frame_processor;
  CapturedFrameSink captured_frame_sink;
  PreviewSamplingHook preview_sampling_hook;
  preview::LatestFrameSampler sampler;

  std::mutex state_mutex;
  daheng::DahengDiagnostics diagnostics;
  bool connected = false;
  std::string failure;
  std::chrono::steady_clock::time_point rate_started_at;
  std::uint64_t frames_in_rate_window = 0;
  double observed_frames_per_second = 0.0;
  std::uint64_t latest_capture_frame_id = 0;
  std::chrono::steady_clock::time_point latest_capture_received_at;
  std::uint64_t latest_sink_frame_id = 0;
  std::chrono::steady_clock::time_point latest_sink_completed_at;
  std::uint64_t latest_sampler_frame_id = 0;
  std::chrono::steady_clock::time_point latest_sampler_completed_at;

  std::mutex render_mutex;
  std::shared_ptr<const preview::RenderedPreviewImage> rendered_routine;
  std::shared_ptr<const preview::RenderedPreviewImage> rendered_full_resolution;
  std::uint64_t preview_generation = 0;
  std::condition_variable full_resolution_condition;
  std::mutex render_wait_mutex;
  std::condition_variable_any render_condition;
  bool render_requested = false;
  bool full_resolution_render_requested = false;
  bool has_requested_routine_render = false;
  std::chrono::steady_clock::time_point last_routine_render_requested_at;
  RendererStage renderer_stage = RendererStage::kIdle;
  std::atomic<bool> renderer_failed = false;

  std::mutex command_mutex;
  std::deque<std::shared_ptr<SettingsCommand>> commands;
  bool accepting_commands = false;
  bool settings_pending = false;

  std::jthread worker;
  std::jthread renderer;
};

CameraWorker::CameraWorker(CameraRole role, std::unique_ptr<PreviewCameraDevice> camera,
                           daheng::DahengConfiguration configuration,
                           std::unique_ptr<preview::PreviewFrameProcessor> frame_processor,
                           CapturedFrameSink captured_frame_sink,
                           PreviewSamplingHook preview_sampling_hook)
    : impl_(std::make_unique<Impl>(role, std::move(camera), configuration,
                                   std::move(frame_processor), std::move(captured_frame_sink),
                                   std::move(preview_sampling_hook))) {}

CameraWorker::~CameraWorker() { Stop(); }

void CameraWorker::Start() { impl_->Start(); }

void CameraWorker::Stop() noexcept { impl_->Stop(); }

CameraStatus CameraWorker::Status() { return impl_->Status(); }

std::uint64_t CameraWorker::TimestampTicksPerSecond() { return impl_->TimestampTicksPerSecond(); }

std::shared_ptr<const preview::SampledPreviewFrame> CameraWorker::LatestSampledFrame() {
  return impl_->sampler.Latest();
}

void CameraWorker::SetLatestFrameSamplingInterval(
    std::chrono::steady_clock::duration minimum_interval) {
  impl_->sampler.SetMinimumInterval(minimum_interval);
}

std::chrono::steady_clock::duration CameraWorker::LatestFrameSamplingInterval() const {
  return impl_->sampler.minimum_interval();
}

std::optional<PreviewImage> CameraWorker::LatestPreview(bool full_resolution) {
  return impl_->LatestPreview(full_resolution);
}

CameraStatus CameraWorker::UpdateSettings(const CameraSettingsUpdate &settings) {
  return impl_->UpdateSettings(settings);
}

}  // namespace swing_capture::service
