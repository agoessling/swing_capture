#include "capture/service/synthetic_swing_hil_operation.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace swing_capture::service {
namespace {

std::string CurrentExceptionMessage() {
  try {
    throw;
  } catch (const std::exception &error) {
    return error.what();
  } catch (...) {
    return "non-standard synthetic swing HIL failure";
  }
}

void ValidateHooks(const SyntheticSwingHilOperationHooks &hooks) {
  if (!hooks.calibrate_brightness || !hooks.arm_capture || !hooks.capture_status ||
      !hooks.run_stimulus || !hooks.validate_completion || !hooks.disarm_capture) {
    throw std::invalid_argument("synthetic swing HIL operation hooks are incomplete");
  }
}

void AppendCleanupFailure(std::string_view label, std::string error, std::string *failure) {
  if (!failure->empty()) {
    *failure += "; ";
  }
  *failure += std::string(label) + ": " + std::move(error);
}

void SleepUntilNextPoll(const std::stop_token &stop_token,
                        std::chrono::steady_clock::duration interval) {
  std::condition_variable_any condition;
  std::mutex mutex;
  std::unique_lock lock(mutex);
  condition.wait_for(lock, stop_token, interval, [] { return false; });
}

}  // namespace

std::string_view SyntheticSwingHilStageName(SyntheticSwingHilStage stage) noexcept {
  switch (stage) {
    case SyntheticSwingHilStage::kIdle:
      return "idle";
    case SyntheticSwingHilStage::kCalibrating:
      return "calibrating";
    case SyntheticSwingHilStage::kArming:
      return "arming";
    case SyntheticSwingHilStage::kStimulus:
      return "stimulus";
    case SyntheticSwingHilStage::kCapturing:
      return "capturing";
    case SyntheticSwingHilStage::kEncoding:
      return "encoding";
    case SyntheticSwingHilStage::kReady:
      return "ready";
    case SyntheticSwingHilStage::kError:
      return "error";
  }
  return "error";
}

SyntheticSwingHilOperation::SyntheticSwingHilOperation(SyntheticSwingHilOperationConfig config,
                                                       SyntheticSwingHilOperationHooks hooks)
    : config_(config), hooks_(std::move(hooks)) {
  ValidateHooks(hooks_);
  if (config_.arm_timeout <= std::chrono::steady_clock::duration::zero() ||
      config_.completion_timeout <= std::chrono::steady_clock::duration::zero() ||
      config_.poll_interval <= std::chrono::steady_clock::duration::zero()) {
    throw std::invalid_argument("synthetic swing HIL operation timeouts must be positive");
  }
}

SyntheticSwingHilOperation::~SyntheticSwingHilOperation() { StopAndJoin(); }

void SyntheticSwingHilOperation::Start() {
  const std::scoped_lock start_lock(start_mutex_);
  {
    const std::scoped_lock lock(mutex_);
    if (status_.busy) {
      throw std::logic_error("synthetic swing HIL is already running");
    }
  }
  const SyntheticSwingCaptureStatus capture = hooks_.capture_status();
  if (capture.state != SyntheticSwingCaptureState::kSetup &&
      capture.state != SyntheticSwingCaptureState::kReady) {
    throw std::logic_error("synthetic swing HIL requires setup or ready capture state");
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  {
    const std::scoped_lock lock(mutex_);
    status_.busy = true;
    status_.stage = SyntheticSwingHilStage::kCalibrating;
    status_.error.clear();
    status_.session_id.reset();
    status_.selected_brightness.reset();
  }
  worker_ = std::jthread([this](const std::stop_token &stop_token) { Run(stop_token); });
}

SyntheticSwingHilOperationStatus SyntheticSwingHilOperation::Status() const {
  const std::scoped_lock lock(mutex_);
  return status_;
}

void SyntheticSwingHilOperation::Run(const std::stop_token &stop_token) noexcept {
  SyntheticSwingCaptureStatus completed;
  bool capture_completed = false;
  std::string failure;
  try {
    const std::uint8_t brightness = hooks_.calibrate_brightness(stop_token);
    if (brightness == 0U) {
      throw std::runtime_error("synthetic swing brightness calibration selected zero");
    }
    {
      const std::scoped_lock lock(mutex_);
      status_.selected_brightness = brightness;
    }
    SetStage(SyntheticSwingHilStage::kArming);
    hooks_.arm_capture();
    static_cast<void>(
        WaitForArmed(stop_token, std::chrono::steady_clock::now() + config_.arm_timeout));
    SetStage(SyntheticSwingHilStage::kStimulus);
    hooks_.run_stimulus(brightness, stop_token);
    SetStage(SyntheticSwingHilStage::kCapturing, hooks_.capture_status());
    completed = WaitForCompletion(stop_token,
                                  std::chrono::steady_clock::now() + config_.completion_timeout);
    capture_completed = true;
    hooks_.validate_completion(completed);
  } catch (...) {
    failure = CurrentExceptionMessage();
    try {
      hooks_.disarm_capture();
    } catch (...) {
      AppendCleanupFailure("capture cleanup failed", CurrentExceptionMessage(), &failure);
    }
  }
  if (!failure.empty()) {
    RecordFailure(std::move(failure));
    return;
  }
  if (!capture_completed) {
    RecordFailure("synthetic swing HIL completed without terminal capture status");
    return;
  }
  RecordSuccess(completed);
}

void SyntheticSwingHilOperation::SetStage(SyntheticSwingHilStage stage,
                                          const SyntheticSwingCaptureStatus &capture) {
  const std::scoped_lock lock(mutex_);
  status_.stage = stage;
  status_.session_id = capture.session_id;
}

SyntheticSwingCaptureStatus SyntheticSwingHilOperation::WaitForArmed(
    const std::stop_token &stop_token, std::chrono::steady_clock::time_point deadline) const {
  while (!stop_token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
    SyntheticSwingCaptureStatus capture = hooks_.capture_status();
    if (capture.state == SyntheticSwingCaptureState::kArmed) {
      return capture;
    }
    if (capture.state == SyntheticSwingCaptureState::kError) {
      throw std::runtime_error(capture.error.empty() ? "capture failed while arming"
                                                     : capture.error);
    }
    SleepUntilNextPoll(stop_token, config_.poll_interval);
  }
  throw std::runtime_error(stop_token.stop_requested() ? "synthetic swing HIL stopped while arming"
                                                       : "synthetic swing HIL arming timed out");
}

SyntheticSwingCaptureStatus SyntheticSwingHilOperation::WaitForCompletion(
    const std::stop_token &stop_token, std::chrono::steady_clock::time_point deadline) {
  while (!stop_token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
    SyntheticSwingCaptureStatus capture = hooks_.capture_status();
    if (capture.state == SyntheticSwingCaptureState::kReady) {
      if (!capture.session_id.has_value()) {
        throw std::runtime_error("synthetic swing capture completed without a session ID");
      }
      return capture;
    }
    if (capture.state == SyntheticSwingCaptureState::kError) {
      throw std::runtime_error(capture.error.empty() ? "synthetic swing capture failed"
                                                     : capture.error);
    }
    SetStage(capture.state == SyntheticSwingCaptureState::kEncoding
                 ? SyntheticSwingHilStage::kEncoding
                 : SyntheticSwingHilStage::kCapturing,
             capture);
    SleepUntilNextPoll(stop_token, config_.poll_interval);
  }
  throw std::runtime_error(stop_token.stop_requested()
                               ? "synthetic swing HIL stopped before publication"
                               : "synthetic swing HIL publication timed out");
}

void SyntheticSwingHilOperation::RecordSuccess(const SyntheticSwingCaptureStatus &capture) {
  const std::scoped_lock lock(mutex_);
  status_.busy = false;
  status_.stage = SyntheticSwingHilStage::kReady;
  status_.session_id = capture.session_id;
  status_.last_stage = SyntheticSwingHilStage::kReady;
  status_.last_error.clear();
  status_.last_session_id = capture.session_id;
  status_.last_selected_brightness = status_.selected_brightness;
}

void SyntheticSwingHilOperation::RecordFailure(std::string error) noexcept {
  const std::scoped_lock lock(mutex_);
  status_.busy = false;
  status_.stage = SyntheticSwingHilStage::kError;
  status_.error = error;
  status_.last_stage = SyntheticSwingHilStage::kError;
  status_.last_error = std::move(error);
  status_.last_session_id = status_.session_id;
  status_.last_selected_brightness = status_.selected_brightness;
}

void SyntheticSwingHilOperation::StopAndJoin() noexcept {
  if (!worker_.joinable()) {
    return;
  }
  worker_.request_stop();
  worker_.join();
}

}  // namespace swing_capture::service
