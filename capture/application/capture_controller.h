#ifndef SWING_CAPTURE_CAPTURE_APPLICATION_CAPTURE_CONTROLLER_H_
#define SWING_CAPTURE_CAPTURE_APPLICATION_CAPTURE_CONTROLLER_H_

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "capture/application/audio_impact_monitor.h"
#include "capture/application/camera_clip_buffer.h"
#include "capture/audio/audio_capture_session.h"
#include "capture/core/pooled_raw_frame_ring.h"
#include "capture/trigger/impact_detector.h"

namespace swing_capture::application {

inline constexpr auto kDefaultCapturePreRoll = std::chrono::milliseconds(1400);
inline constexpr auto kDefaultCapturePostRoll = std::chrono::milliseconds(500);

enum class CaptureApplicationState {
  kSetup,
  kArming,
  kArmed,
  kWaitingPostRoll,
  kEncoding,
  kReady,
  kError,
};

[[nodiscard]] std::string_view CaptureApplicationStateName(CaptureApplicationState state) noexcept;

enum class CaptureTriggerSource {
  kAudio,
  kManual,
};

[[nodiscard]] std::string_view CaptureTriggerSourceName(CaptureTriggerSource source) noexcept;

struct CaptureControllerConfig {
  std::chrono::steady_clock::duration pre_roll = kDefaultCapturePreRoll;
  std::chrono::steady_clock::duration post_roll = kDefaultCapturePostRoll;
  std::chrono::steady_clock::duration frame_boundary_margin = std::chrono::milliseconds(5);
  std::size_t minimum_pre_roll_frames = 300;
};

struct CameraCaptureEndpoint {
  std::string role;
  std::string serial;
  std::uint64_t device_ticks_per_second = 0;
  CameraClipBuffer *buffer = nullptr;
};

struct SessionIdentity {
  std::string session_id;
  std::string created_at_utc;
};

using SessionIdentityFactory = std::function<SessionIdentity()>;

struct CapturedTrigger {
  CaptureTriggerSource source = CaptureTriggerSource::kAudio;
  ImpactEvent impact;
};

struct CapturedCameraWindow {
  std::string role;
  std::string serial;
  std::uint64_t device_ticks_per_second = 0;
  PooledRawFrameSnapshot frames;
};

// Host-monotonic boundaries for the latency-critical path after an impact is
// accepted. Raw time points stay process-local; the publisher serializes only
// relative durations so a retained session remains portable.
struct CapturePipelineTiming {
  std::chrono::steady_clock::time_point trigger_accepted_at;
  std::chrono::steady_clock::time_point freeze_target_at;
  std::chrono::steady_clock::time_point freeze_started_at;
  std::chrono::steady_clock::time_point freeze_completed_at;
  std::chrono::steady_clock::time_point audio_stop_started_at;
  std::chrono::steady_clock::time_point audio_stop_completed_at;
};

struct CapturedSession {
  SessionIdentity identity;
  CapturedTrigger trigger;
  std::array<CapturedCameraWindow, 2> cameras;
  CapturePipelineTiming pipeline_timing;
};

struct PublishedSession {
  std::string session_id;
  std::string created_at_utc;
  std::string manifest_path;
};

using SessionPublisher = std::function<PublishedSession(CapturedSession)>;

struct CaptureControllerStatus {
  CaptureApplicationState state = CaptureApplicationState::kSetup;
  bool armed = false;
  std::optional<std::string> active_session_id;
  std::optional<SessionIdentity> active_session_identity;
  std::optional<CapturedTrigger> last_trigger;
  std::string error;
  AudioImpactMonitorStatus audio;
  std::array<CameraClipBufferStatus, 2> cameras;
  std::vector<PublishedSession> sessions;
};

// Coordinates arming, microphone impact delivery, post-roll retention, and a
// synchronous session publisher on a dedicated control/encoding thread.
// Camera owner threads only call CameraClipBuffer::ObserveFrame; the audio
// thread only calls SubmitImpact through a short critical section.
class CaptureController final {
 public:
  CaptureController(CaptureControllerConfig config, std::array<CameraCaptureEndpoint, 2> cameras,
                    AudioCaptureSourceFactory audio_source_factory,
                    std::uint32_t audio_sample_rate_hz, SessionIdentityFactory identity_factory,
                    SessionPublisher publisher, ImpactDetectorConfig detector_config = {});
  ~CaptureController();

  CaptureController(const CaptureController &) = delete;
  CaptureController &operator=(const CaptureController &) = delete;
  CaptureController(CaptureController &&) = delete;
  CaptureController &operator=(CaptureController &&) = delete;

  void Arm();
  void Disarm() noexcept;
  [[nodiscard]] SessionIdentity CaptureManually();
  [[nodiscard]] CaptureControllerStatus Status() const;

 private:
  [[nodiscard]] std::optional<SessionIdentity> SubmitImpact(CaptureTriggerSource source,
                                                            const ImpactEvent &impact);
  void Run(const std::stop_token &stop_token) noexcept;
  void AdvanceArming(std::unique_lock<std::mutex> &lock, const std::stop_token &stop_token);
  void PublishPending(std::unique_lock<std::mutex> &lock, const std::stop_token &stop_token);
  void RecordFailure(std::string message) noexcept;

  CaptureControllerConfig config_;
  std::array<CameraCaptureEndpoint, 2> cameras_;
  SessionIdentityFactory identity_factory_;
  SessionPublisher publisher_;
  std::unique_ptr<AudioImpactMonitor> audio_monitor_;

  std::mutex lifecycle_mutex_;
  mutable std::mutex mutex_;
  std::condition_variable_any changed_;
  CaptureApplicationState state_ = CaptureApplicationState::kSetup;
  std::chrono::steady_clock::time_point arm_ready_at_;
  std::chrono::steady_clock::time_point freeze_at_;
  std::optional<CapturedTrigger> pending_trigger_;
  std::optional<CapturedTrigger> last_trigger_;
  std::optional<SessionIdentity> pending_identity_;
  std::optional<std::chrono::steady_clock::time_point> trigger_accepted_at_;
  std::optional<std::string> active_session_id_;
  std::string error_;
  std::vector<PublishedSession> sessions_;
  std::jthread worker_;
};

}  // namespace swing_capture::application

#endif  // SWING_CAPTURE_CAPTURE_APPLICATION_CAPTURE_CONTROLLER_H_
