#include "capture/application/capture_controller.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "capture/application/audio_impact_monitor.h"
#include "capture/application/camera_clip_buffer.h"
#include "capture/audio/audio_capture_session.h"
#include "capture/core/pooled_raw_frame_ring.h"
#include "capture/trigger/impact_detector.h"

namespace swing_capture::application {
namespace {

std::string CurrentExceptionMessage() {
  try {
    throw;
  } catch (const std::exception &error) {
    return error.what();
  } catch (...) {
    return "non-standard capture controller failure";
  }
}

void ValidateConfig(const CaptureControllerConfig &config,
                    const std::array<CameraCaptureEndpoint, 2> &cameras) {
  if (config.pre_roll <= std::chrono::steady_clock::duration::zero() ||
      config.post_roll < std::chrono::steady_clock::duration::zero() ||
      config.frame_boundary_margin < std::chrono::steady_clock::duration::zero() ||
      config.minimum_pre_roll_frames == 0) {
    throw std::invalid_argument("capture controller timing and retention must be positive");
  }
  for (const CameraCaptureEndpoint &camera : cameras) {
    if (camera.role.empty() || camera.serial.empty() || camera.device_ticks_per_second == 0 ||
        camera.buffer == nullptr) {
      throw std::invalid_argument("capture controller camera endpoint is incomplete");
    }
  }
  if (cameras[0].role == cameras[1].role || cameras[0].serial == cameras[1].serial) {
    throw std::invalid_argument("capture controller camera roles and serials must be distinct");
  }
}

ImpactEvent ManualImpact(std::chrono::steady_clock::time_point now) {
  return {
      .strike_time = now,
      .confirmation_time = now,
      .source_block_start = now,
      .sample_index_in_block = 0,
      .sample_rate_hz = 0,
      .peak_amplitude = 0.0F,
      .noise_floor_at_detection = 0.0F,
      .threshold_at_detection = 0.0F,
  };
}

}  // namespace

std::string_view CaptureApplicationStateName(CaptureApplicationState state) noexcept {
  switch (state) {
    case CaptureApplicationState::kSetup:
      return "setup";
    case CaptureApplicationState::kArming:
      return "arming";
    case CaptureApplicationState::kArmed:
      return "armed";
    case CaptureApplicationState::kWaitingPostRoll:
      return "waiting_post_roll";
    case CaptureApplicationState::kEncoding:
      return "encoding";
    case CaptureApplicationState::kReady:
      return "ready";
    case CaptureApplicationState::kError:
      return "error";
  }
  return "unknown";
}

std::string_view CaptureTriggerSourceName(CaptureTriggerSource source) noexcept {
  switch (source) {
    case CaptureTriggerSource::kAudio:
      return "audio";
    case CaptureTriggerSource::kManual:
      return "manual";
  }
  return "unknown";
}

CaptureController::CaptureController(CaptureControllerConfig config,
                                     std::array<CameraCaptureEndpoint, 2> cameras,
                                     AudioCaptureSourceFactory audio_source_factory,
                                     std::uint32_t audio_sample_rate_hz,
                                     SessionIdentityFactory identity_factory,
                                     SessionPublisher publisher,
                                     ImpactDetectorConfig detector_config)
    : config_(config),
      cameras_(std::move(cameras)),
      identity_factory_(std::move(identity_factory)),
      publisher_(std::move(publisher)) {
  ValidateConfig(config_, cameras_);
  if (!identity_factory_ || !publisher_) {
    throw std::invalid_argument("capture controller requires identity and publisher callbacks");
  }
  audio_monitor_ = std::make_unique<AudioImpactMonitor>(
      std::move(audio_source_factory), audio_sample_rate_hz,
      [this](const ImpactEvent &impact) {
        static_cast<void>(SubmitImpact(CaptureTriggerSource::kAudio, impact));
      },
      detector_config);
  worker_ = std::jthread([this](const std::stop_token &stop_token) { Run(stop_token); });
}

CaptureController::~CaptureController() {
  Disarm();
  worker_.request_stop();
  changed_.notify_all();
  worker_.join();
}

void CaptureController::Arm() {
  const std::scoped_lock lifecycle_lock(lifecycle_mutex_);
  std::unique_lock lock(mutex_);
  if (state_ != CaptureApplicationState::kSetup && state_ != CaptureApplicationState::kReady &&
      state_ != CaptureApplicationState::kError) {
    throw std::logic_error("capture application is already armed; disarm before retrying");
  }
  error_.clear();
  active_session_id_.reset();
  pending_trigger_.reset();
  pending_identity_.reset();
  trigger_accepted_at_.reset();
  try {
    cameras_[0].buffer->Arm();
    cameras_[1].buffer->Arm();
    audio_monitor_->Start();
  } catch (...) {
    lock.unlock();
    audio_monitor_->Stop();
    cameras_[0].buffer->Disarm();
    cameras_[1].buffer->Disarm();
    RecordFailure(CurrentExceptionMessage());
    throw;
  }
  arm_ready_at_ =
      std::chrono::steady_clock::now() + config_.pre_roll + config_.frame_boundary_margin;
  state_ = CaptureApplicationState::kArming;
  lock.unlock();
  changed_.notify_all();
}

void CaptureController::Disarm() noexcept {
  const std::scoped_lock lifecycle_lock(lifecycle_mutex_);
  audio_monitor_->Stop();
  cameras_[0].buffer->Disarm();
  cameras_[1].buffer->Disarm();
  {
    const std::scoped_lock lock(mutex_);
    state_ = CaptureApplicationState::kSetup;
    pending_trigger_.reset();
    pending_identity_.reset();
    trigger_accepted_at_.reset();
    active_session_id_.reset();
    error_.clear();
  }
  changed_.notify_all();
}

SessionIdentity CaptureController::CaptureManually() {
  const std::scoped_lock lifecycle_lock(lifecycle_mutex_);
  const auto now = std::chrono::steady_clock::now();
  const auto accepted = SubmitImpact(CaptureTriggerSource::kManual, ManualImpact(now));
  if (!accepted.has_value()) {
    throw std::logic_error("manual capture was not accepted");
  }
  return *accepted;
}

CaptureControllerStatus CaptureController::Status() const {
  CaptureControllerStatus status;
  {
    const std::scoped_lock lock(mutex_);
    status.state = state_;
    status.armed = state_ == CaptureApplicationState::kArming ||
                   state_ == CaptureApplicationState::kArmed ||
                   state_ == CaptureApplicationState::kWaitingPostRoll ||
                   state_ == CaptureApplicationState::kEncoding;
    status.active_session_id = active_session_id_;
    status.active_session_identity = pending_identity_;
    status.last_trigger = last_trigger_;
    status.error = error_;
    status.sessions = sessions_;
  }
  status.audio = audio_monitor_->Status();
  for (std::size_t index = 0; index < cameras_.size(); ++index) {
    status.cameras[index] = cameras_[index].buffer->Status();
  }
  return status;
}

std::optional<SessionIdentity> CaptureController::SubmitImpact(CaptureTriggerSource source,
                                                               const ImpactEvent &impact) {
  const std::scoped_lock lock(mutex_);
  if (state_ != CaptureApplicationState::kArmed) {
    return std::nullopt;
  }
  pending_trigger_ = CapturedTrigger{.source = source, .impact = impact};
  last_trigger_ = pending_trigger_;
  pending_identity_ = identity_factory_();
  trigger_accepted_at_ = std::chrono::steady_clock::now();
  active_session_id_ = pending_identity_->session_id;
  freeze_at_ = impact.strike_time + config_.post_roll + config_.frame_boundary_margin;
  state_ = CaptureApplicationState::kWaitingPostRoll;
  // This controller is deliberately one-shot. Once the trigger is accepted,
  // no later microphone event can affect the retained window, so let ALSA
  // shutdown overlap the required camera post-roll instead of joining it on
  // the publication critical path.
  audio_monitor_->RequestStop();
  changed_.notify_all();
  return pending_identity_;
}

void CaptureController::Run(const std::stop_token &stop_token) noexcept {
  std::unique_lock lock(mutex_);
  while (!stop_token.stop_requested()) {
    try {
      changed_.wait(lock, stop_token, [this] {
        return state_ == CaptureApplicationState::kArming ||
               state_ == CaptureApplicationState::kWaitingPostRoll;
      });
      if (stop_token.stop_requested()) {
        return;
      }
      if (state_ == CaptureApplicationState::kArming) {
        AdvanceArming(lock, stop_token);
      } else if (state_ == CaptureApplicationState::kWaitingPostRoll) {
        PublishPending(lock, stop_token);
      }
    } catch (...) {
      if (lock.owns_lock()) {
        lock.unlock();
      }
      {
        const std::scoped_lock lifecycle_lock(lifecycle_mutex_);
        audio_monitor_->Stop();
        cameras_[0].buffer->Disarm();
        cameras_[1].buffer->Disarm();
        RecordFailure(CurrentExceptionMessage());
      }
      if (!lock.owns_lock()) {
        lock.lock();
      }
    }
  }
}

void CaptureController::AdvanceArming(std::unique_lock<std::mutex> &lock,
                                      const std::stop_token &stop_token) {
  changed_.wait_until(lock, stop_token, arm_ready_at_,
                      [this] { return state_ != CaptureApplicationState::kArming; });
  if (stop_token.stop_requested() || state_ != CaptureApplicationState::kArming) {
    return;
  }
  lock.unlock();
  const bool ready = std::ranges::all_of(cameras_, [this](const CameraCaptureEndpoint &camera) {
    const CameraClipBufferStatus status = camera.buffer->Status();
    return status.armed && status.failures == 0 &&
           status.retained_frames >= config_.minimum_pre_roll_frames;
  });
  lock.lock();
  if (state_ != CaptureApplicationState::kArming) {
    return;
  }
  if (!ready) {
    throw std::runtime_error("camera rings did not fill the required pre-roll while arming");
  }
  state_ = CaptureApplicationState::kArmed;
}

void CaptureController::PublishPending(std::unique_lock<std::mutex> &lock,
                                       const std::stop_token &stop_token) {
  changed_.wait_until(lock, stop_token, freeze_at_,
                      [this] { return state_ != CaptureApplicationState::kWaitingPostRoll; });
  if (stop_token.stop_requested() || state_ != CaptureApplicationState::kWaitingPostRoll) {
    return;
  }
  if (!pending_trigger_.has_value() || !pending_identity_.has_value() ||
      !trigger_accepted_at_.has_value()) {
    throw std::logic_error("waiting capture is missing trigger or session identity");
  }
  const CapturedTrigger trigger = pending_trigger_.value();
  const SessionIdentity identity = pending_identity_.value();
  const auto trigger_accepted_at = trigger_accepted_at_.value();
  lock.unlock();
  const std::scoped_lock lifecycle_lock(lifecycle_mutex_);
  lock.lock();
  if (state_ != CaptureApplicationState::kWaitingPostRoll) {
    return;
  }
  state_ = CaptureApplicationState::kEncoding;
  lock.unlock();
  const auto freeze_started_at = std::chrono::steady_clock::now();
  PooledRawFrameSnapshot down_the_line_frames = cameras_[0].buffer->FreezeAndRotate();
  PooledRawFrameSnapshot face_on_frames = cameras_[1].buffer->FreezeAndRotate();
  const auto freeze_completed_at = std::chrono::steady_clock::now();
  const auto audio_stop_started_at = freeze_completed_at;
  audio_monitor_->Stop();
  const auto audio_stop_completed_at = std::chrono::steady_clock::now();
  CapturedSession captured = {
      .identity = identity,
      .trigger = trigger,
      .cameras =
          std::array{
              CapturedCameraWindow{.role = cameras_[0].role,
                                   .serial = cameras_[0].serial,
                                   .device_ticks_per_second = cameras_[0].device_ticks_per_second,
                                   .frames = std::move(down_the_line_frames)},
              CapturedCameraWindow{.role = cameras_[1].role,
                                   .serial = cameras_[1].serial,
                                   .device_ticks_per_second = cameras_[1].device_ticks_per_second,
                                   .frames = std::move(face_on_frames)},
          },
      .pipeline_timing =
          CapturePipelineTiming{
              .trigger_accepted_at = trigger_accepted_at,
              .freeze_target_at = freeze_at_,
              .freeze_started_at = freeze_started_at,
              .freeze_completed_at = freeze_completed_at,
              .audio_stop_started_at = audio_stop_started_at,
              .audio_stop_completed_at = audio_stop_completed_at,
          },
  };
  PublishedSession published = publisher_(std::move(captured));
  cameras_[0].buffer->Disarm();
  cameras_[1].buffer->Disarm();
  lock.lock();
  if (state_ != CaptureApplicationState::kEncoding) {
    return;
  }
  sessions_.insert(sessions_.begin(), std::move(published));
  pending_trigger_.reset();
  pending_identity_.reset();
  trigger_accepted_at_.reset();
  state_ = CaptureApplicationState::kReady;
}

void CaptureController::RecordFailure(std::string message) noexcept {
  const std::scoped_lock lock(mutex_);
  error_ = std::move(message);
  state_ = CaptureApplicationState::kError;
  pending_trigger_.reset();
  trigger_accepted_at_.reset();
  changed_.notify_all();
}

}  // namespace swing_capture::application
