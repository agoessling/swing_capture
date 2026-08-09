#ifndef SWING_CAPTURE_CAPTURE_AUDIO_AUDIO_CAPTURE_SESSION_H_
#define SWING_CAPTURE_CAPTURE_AUDIO_AUDIO_CAPTURE_SESSION_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "capture/audio/arecord_pcm_source.h"

namespace swing_capture {

// Minimal injectable source boundary. A session invokes the factory and every
// method, including destruction, on its capture worker thread.
class AudioCaptureSource {
 public:
  virtual ~AudioCaptureSource() = default;

  AudioCaptureSource(const AudioCaptureSource &) = delete;
  AudioCaptureSource &operator=(const AudioCaptureSource &) = delete;
  AudioCaptureSource(AudioCaptureSource &&) = delete;
  AudioCaptureSource &operator=(AudioCaptureSource &&) = delete;

  [[nodiscard]] virtual bool Start(std::string *error) = 0;
  [[nodiscard]] virtual PcmReadResult ReadBlock() = 0;
  virtual void Stop() noexcept = 0;

 protected:
  AudioCaptureSource() = default;
};

using AudioCaptureSourceFactory = std::function<std::unique_ptr<AudioCaptureSource>()>;

struct AudioCaptureDiagnostics {
  bool source_started = false;
  bool stop_requested = false;
  bool reached_end_of_stream = false;
  bool continuity_valid = true;
  bool clean_shutdown = false;
  std::uint64_t completed_blocks = 0;
  std::uint64_t completed_samples = 0;
  std::chrono::nanoseconds worker_elapsed = std::chrono::nanoseconds::zero();
  std::string error;
};

struct AudioCaptureResult {
  std::vector<std::int16_t> samples;
  AudioCaptureDiagnostics diagnostics;
};

struct AudioCaptureWaitResult {
  bool condition_satisfied = false;
  bool terminal = false;
  bool deadline_expired = false;
  bool joined = false;
  std::uint64_t completed_samples = 0;
  std::string error;
};

// Owns one continuous mono capture. Lifecycle methods are intended for one
// control thread; completed_sample_count() and the wait methods are safe while
// the worker is running. The captured vector is unavailable until the worker
// has terminated and been joined.
class AudioCaptureSession final {
 public:
  explicit AudioCaptureSession(AudioCaptureSourceFactory source_factory);
  ~AudioCaptureSession();

  AudioCaptureSession(const AudioCaptureSession &) = delete;
  AudioCaptureSession &operator=(const AudioCaptureSession &) = delete;
  AudioCaptureSession(AudioCaptureSession &&) = delete;
  AudioCaptureSession &operator=(AudioCaptureSession &&) = delete;

  void Start();

  [[nodiscard]] AudioCaptureWaitResult WaitUntilReady(
      std::chrono::steady_clock::time_point deadline) const;
  [[nodiscard]] AudioCaptureWaitResult WaitForCompletedSamples(
      std::uint64_t minimum_samples, std::chrono::steady_clock::time_point deadline) const;

  // Verifies that capture remained alive through an absolute host time. The
  // separate wait deadline lets callers bound the operation even when the
  // requested capture-through time is accidentally too distant.
  [[nodiscard]] AudioCaptureWaitResult WaitThroughWallTime(
      std::chrono::steady_clock::time_point capture_through,
      std::chrono::steady_clock::time_point wait_deadline) const;

  [[nodiscard]] AudioCaptureWaitResult WaitUntilTerminal(
      std::chrono::steady_clock::time_point deadline) const;

  void RequestStop() noexcept;

  // Requests shutdown, waits only until deadline, and joins only after the
  // worker has reported terminal. A timed-out call can be retried.
  [[nodiscard]] AudioCaptureWaitResult RequestStopAndJoin(
      std::chrono::steady_clock::time_point deadline);

  [[nodiscard]] std::uint64_t completed_sample_count() const noexcept;
  [[nodiscard]] bool started() const noexcept;
  [[nodiscard]] bool joined() const noexcept;

  // Returns a safe copy once the worker has destroyed its source and published
  // terminal, even if a previous bounded join attempt timed out. Returns null
  // immediately while capture can still mutate the result.
  [[nodiscard]] std::optional<AudioCaptureResult> CopyTerminalResult() const;

  // Throws until RequestStopAndJoin has successfully joined the worker.
  [[nodiscard]] const AudioCaptureResult &result() const;

 private:
  struct State;

  static void RunWorker(const std::shared_ptr<State> &state,
                        const AudioCaptureSourceFactory &source_factory) noexcept;
  static void SetError(const std::shared_ptr<State> &state, std::string error);
  static bool StartSource(const std::shared_ptr<State> &state, AudioCaptureSource *source);
  static void CaptureBlocks(const std::shared_ptr<State> &state, AudioCaptureSource *source);
  static bool ProcessReadResult(const std::shared_ptr<State> &state, PcmReadResult read_result);
  static bool AppendBlock(const std::shared_ptr<State> &state, MonoPcmBlock block);
  static void Finalize(const std::shared_ptr<State> &state,
                       std::chrono::steady_clock::time_point worker_start);
  void RequireStarted() const;

  AudioCaptureSourceFactory source_factory_;
  std::shared_ptr<State> state_;
  std::thread worker_;
  bool started_ = false;
  bool joined_ = false;
};

// The returned factory captures configuration only. ArecordPcmSource itself is
// not constructed until AudioCaptureSession invokes the factory on its worker.
[[nodiscard]] AudioCaptureSourceFactory MakeArecordAudioCaptureSourceFactory(
    ArecordPcmConfig config);

}  // namespace swing_capture

#endif  // SWING_CAPTURE_CAPTURE_AUDIO_AUDIO_CAPTURE_SESSION_H_
