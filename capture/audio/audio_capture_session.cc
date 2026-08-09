#include "capture/audio/audio_capture_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "capture/audio/arecord_pcm_source.h"

namespace swing_capture {
namespace {

class ArecordAudioCaptureSource final : public AudioCaptureSource {
 public:
  explicit ArecordAudioCaptureSource(ArecordPcmConfig config) : source_(std::move(config)) {}

  [[nodiscard]] bool Start(std::string *error) override { return source_.Start(error); }
  [[nodiscard]] PcmReadResult ReadBlock() override { return source_.ReadBlock(); }
  void Stop() noexcept override { source_.Stop(); }

 private:
  ArecordPcmSource source_;
};

}  // namespace

struct AudioCaptureSession::State {
  mutable std::mutex mutex;
  mutable std::condition_variable changed;
  std::atomic<bool> stop_requested = false;
  std::atomic<std::uint64_t> completed_samples = 0;
  bool ready = false;
  bool terminal = false;
  std::chrono::steady_clock::time_point terminal_time;
  std::string error;
  AudioCaptureResult result;
};

AudioCaptureSession::AudioCaptureSession(AudioCaptureSourceFactory source_factory)
    : source_factory_(std::move(source_factory)), state_(std::make_shared<State>()) {
  if (!source_factory_) {
    throw std::invalid_argument("audio capture source factory cannot be empty");
  }
}

AudioCaptureSession::~AudioCaptureSession() {
  RequestStop();
  if (worker_.joinable()) {
    worker_.join();
    joined_ = true;
  }
}

void AudioCaptureSession::Start() {
  if (started_) {
    throw std::logic_error("audio capture session has already been started");
  }
  started_ = true;
  try {
    worker_ = std::thread(&AudioCaptureSession::RunWorker, state_, std::move(source_factory_));
  } catch (...) {
    started_ = false;
    throw;
  }
}

AudioCaptureWaitResult AudioCaptureSession::WaitUntilReady(
    std::chrono::steady_clock::time_point deadline) const {
  RequireStarted();
  std::unique_lock lock(state_->mutex);
  state_->changed.wait_until(lock, deadline, [this] { return state_->ready || state_->terminal; });
  const bool condition_satisfied = state_->ready;
  return {
      .condition_satisfied = condition_satisfied,
      .terminal = state_->terminal,
      .deadline_expired = !condition_satisfied && !state_->terminal,
      .joined = joined_,
      .completed_samples = state_->completed_samples.load(std::memory_order_acquire),
      .error = state_->error,
  };
}

AudioCaptureWaitResult AudioCaptureSession::WaitForCompletedSamples(
    std::uint64_t minimum_samples, std::chrono::steady_clock::time_point deadline) const {
  RequireStarted();
  std::unique_lock lock(state_->mutex);
  state_->changed.wait_until(lock, deadline, [this, minimum_samples] {
    return state_->completed_samples.load(std::memory_order_acquire) >= minimum_samples ||
           state_->terminal;
  });
  const std::uint64_t completed = state_->completed_samples.load(std::memory_order_acquire);
  const bool condition_satisfied = completed >= minimum_samples;
  return {
      .condition_satisfied = condition_satisfied,
      .terminal = state_->terminal,
      .deadline_expired = !condition_satisfied && !state_->terminal,
      .joined = joined_,
      .completed_samples = completed,
      .error = state_->error,
  };
}

AudioCaptureWaitResult AudioCaptureSession::WaitThroughWallTime(
    std::chrono::steady_clock::time_point capture_through,
    std::chrono::steady_clock::time_point wait_deadline) const {
  RequireStarted();
  std::unique_lock lock(state_->mutex);
  while (!state_->terminal && std::chrono::steady_clock::now() < capture_through &&
         std::chrono::steady_clock::now() < wait_deadline) {
    state_->changed.wait_until(lock, std::min(capture_through, wait_deadline));
  }
  const auto now = std::chrono::steady_clock::now();
  const bool condition_satisfied =
      state_->terminal ? state_->terminal_time >= capture_through : now >= capture_through;
  return {
      .condition_satisfied = condition_satisfied,
      .terminal = state_->terminal,
      .deadline_expired = !condition_satisfied && !state_->terminal && now >= wait_deadline,
      .joined = joined_,
      .completed_samples = state_->completed_samples.load(std::memory_order_acquire),
      .error = state_->error,
  };
}

AudioCaptureWaitResult AudioCaptureSession::WaitUntilTerminal(
    std::chrono::steady_clock::time_point deadline) const {
  RequireStarted();
  std::unique_lock lock(state_->mutex);
  state_->changed.wait_until(lock, deadline, [this] { return state_->terminal; });
  return {
      .condition_satisfied = state_->terminal,
      .terminal = state_->terminal,
      .deadline_expired = !state_->terminal,
      .joined = joined_,
      .completed_samples = state_->completed_samples.load(std::memory_order_acquire),
      .error = state_->error,
  };
}

void AudioCaptureSession::RequestStop() noexcept {
  if (!started_) {
    return;
  }
  state_->stop_requested.store(true, std::memory_order_release);
  state_->changed.notify_all();
}

AudioCaptureWaitResult AudioCaptureSession::RequestStopAndJoin(
    std::chrono::steady_clock::time_point deadline) {
  RequireStarted();
  RequestStop();
  AudioCaptureWaitResult wait = WaitUntilTerminal(deadline);
  if (wait.terminal && worker_.joinable()) {
    worker_.join();
    joined_ = true;
  }
  wait.joined = joined_;
  return wait;
}

std::uint64_t AudioCaptureSession::completed_sample_count() const noexcept {
  return state_->completed_samples.load(std::memory_order_acquire);
}

bool AudioCaptureSession::started() const noexcept { return started_; }

bool AudioCaptureSession::joined() const noexcept { return joined_; }

std::optional<AudioCaptureResult> AudioCaptureSession::CopyTerminalResult() const {
  const std::scoped_lock lock(state_->mutex);
  if (!state_->terminal) {
    return std::nullopt;
  }
  return state_->result;
}

const AudioCaptureResult &AudioCaptureSession::result() const {
  if (!joined_) {
    throw std::logic_error("audio capture result is unavailable until the worker is joined");
  }
  return state_->result;
}

void AudioCaptureSession::RunWorker(const std::shared_ptr<State> &state,
                                    const AudioCaptureSourceFactory &source_factory) noexcept {
  const auto worker_start = std::chrono::steady_clock::now();
  std::unique_ptr<AudioCaptureSource> source;
  try {
    source = source_factory();
    if (!source) {
      throw std::runtime_error("audio capture source factory returned null");
    }
    if (StartSource(state, source.get())) {
      CaptureBlocks(state, source.get());
    }
  } catch (const std::exception &error) {
    SetError(state, "audio capture worker exception: " + std::string(error.what()));
  } catch (...) {
    SetError(state, "audio capture worker failed with an unknown exception");
  }

  if (source) {
    source->Stop();
    source.reset();
  }
  Finalize(state, worker_start);
}

void AudioCaptureSession::SetError(const std::shared_ptr<State> &state, std::string error) {
  const std::scoped_lock lock(state->mutex);
  if (state->error.empty()) {
    state->error = std::move(error);
  }
  state->changed.notify_all();
}

bool AudioCaptureSession::StartSource(const std::shared_ptr<State> &state,
                                      AudioCaptureSource *source) {
  std::string start_error;
  if (!source->Start(&start_error)) {
    SetError(state, start_error.empty() ? "audio capture source failed to start"
                                        : "audio capture source failed to start: " + start_error);
    return false;
  }
  state->result.diagnostics.source_started = true;
  {
    const std::scoped_lock lock(state->mutex);
    state->ready = true;
  }
  state->changed.notify_all();
  return true;
}

void AudioCaptureSession::CaptureBlocks(const std::shared_ptr<State> &state,
                                        AudioCaptureSource *source) {
  while (!state->stop_requested.load(std::memory_order_acquire) &&
         ProcessReadResult(state, source->ReadBlock())) {
  }
}

bool AudioCaptureSession::ProcessReadResult(const std::shared_ptr<State> &state,
                                            PcmReadResult read_result) {
  switch (read_result.status) {
    case PcmReadStatus::kData:
      return AppendBlock(state, std::move(read_result.block));
    case PcmReadStatus::kEndOfStream:
      state->result.diagnostics.reached_end_of_stream = true;
      SetError(state, "audio capture source reached end of stream before shutdown was requested");
      return false;
    case PcmReadStatus::kError:
      SetError(state, read_result.message.empty()
                          ? "audio capture source read failed"
                          : "audio capture source read failed: " + read_result.message);
      return false;
  }
  SetError(state, "audio capture source returned an unknown read status");
  return false;
}

bool AudioCaptureSession::AppendBlock(const std::shared_ptr<State> &state, MonoPcmBlock block) {
  if (block.samples.empty()) {
    SetError(state, "audio capture source returned an empty data block");
    return false;
  }

  const std::uint64_t expected_offset = state->result.samples.size();
  if (block.first_frame_index != expected_offset) {
    state->result.diagnostics.continuity_valid = false;
    SetError(state, "PCM continuity failure: expected block offset " +
                        std::to_string(expected_offset) + ", received " +
                        std::to_string(block.first_frame_index));
    return false;
  }
  if (block.samples.size() >
      std::numeric_limits<std::size_t>::max() - state->result.samples.size()) {
    SetError(state, "PCM capture size overflow");
    return false;
  }

  state->result.samples.insert(state->result.samples.end(), block.samples.begin(),
                               block.samples.end());
  ++state->result.diagnostics.completed_blocks;
  const auto completed = static_cast<std::uint64_t>(state->result.samples.size());
  state->result.diagnostics.completed_samples = completed;
  state->completed_samples.store(completed, std::memory_order_release);
  state->changed.notify_all();
  return true;
}

void AudioCaptureSession::Finalize(const std::shared_ptr<State> &state,
                                   std::chrono::steady_clock::time_point worker_start) {
  const auto terminal_time = std::chrono::steady_clock::now();
  state->result.diagnostics.stop_requested = state->stop_requested.load(std::memory_order_acquire);
  state->result.diagnostics.completed_samples =
      state->completed_samples.load(std::memory_order_acquire);
  state->result.diagnostics.worker_elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(terminal_time - worker_start);
  {
    const std::scoped_lock lock(state->mutex);
    state->result.diagnostics.error = state->error;
    state->result.diagnostics.clean_shutdown = state->error.empty();
    state->terminal_time = terminal_time;
    state->terminal = true;
  }
  state->changed.notify_all();
}

void AudioCaptureSession::RequireStarted() const {
  if (!started_) {
    throw std::logic_error("audio capture session has not been started");
  }
}

AudioCaptureSourceFactory MakeArecordAudioCaptureSourceFactory(ArecordPcmConfig config) {
  return
      [config = std::move(config)] { return std::make_unique<ArecordAudioCaptureSource>(config); };
}

}  // namespace swing_capture
