#ifndef SWING_CAPTURE_CAPTURE_SERVICE_SYNTHETIC_SWING_HIL_OPERATION_H_
#define SWING_CAPTURE_CAPTURE_SERVICE_SYNTHETIC_SWING_HIL_OPERATION_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace swing_capture::service {

enum class SyntheticSwingHilStage {
  kIdle,
  kCalibrating,
  kArming,
  kStimulus,
  kCapturing,
  kEncoding,
  kReady,
  kError,
};

[[nodiscard]] std::string_view SyntheticSwingHilStageName(SyntheticSwingHilStage stage) noexcept;

enum class SyntheticSwingCaptureState {
  kSetup,
  kArming,
  kArmed,
  kWaitingPostRoll,
  kEncoding,
  kReady,
  kError,
};

struct SyntheticSwingCaptureStatus {
  SyntheticSwingCaptureState state = SyntheticSwingCaptureState::kSetup;
  std::optional<std::string> session_id;
  std::string error;
};

struct SyntheticSwingHilOperationStatus {
  bool busy = false;
  SyntheticSwingHilStage stage = SyntheticSwingHilStage::kIdle;
  std::string error;
  std::optional<std::string> session_id;
  std::optional<SyntheticSwingHilStage> last_stage;
  std::string last_error;
  std::optional<std::string> last_session_id;
  std::optional<std::uint8_t> selected_brightness;
  std::optional<std::uint8_t> last_selected_brightness;
};

struct SyntheticSwingHilOperationHooks {
  std::function<std::uint8_t(const std::stop_token &)> calibrate_brightness;
  std::function<void()> arm_capture;
  std::function<SyntheticSwingCaptureStatus()> capture_status;
  std::function<void(std::uint8_t, const std::stop_token &)> run_stimulus;
  std::function<void(const SyntheticSwingCaptureStatus &)> validate_completion;
  std::function<void()> disarm_capture;
};

struct SyntheticSwingHilOperationConfig {
  std::chrono::steady_clock::duration arm_timeout = std::chrono::seconds(3);
  std::chrono::steady_clock::duration completion_timeout = std::chrono::seconds(15);
  std::chrono::steady_clock::duration poll_interval = std::chrono::milliseconds(10);
};

// Owns the explicitly enabled, one-at-a-time HIL lifecycle independently of
// HTTP request threads. Device and capture hooks are injected so lifecycle,
// timeout, and recovery behavior remains hermetically testable.
class SyntheticSwingHilOperation final {
 public:
  SyntheticSwingHilOperation(SyntheticSwingHilOperationConfig config,
                             SyntheticSwingHilOperationHooks hooks);
  ~SyntheticSwingHilOperation();

  SyntheticSwingHilOperation(const SyntheticSwingHilOperation &) = delete;
  SyntheticSwingHilOperation &operator=(const SyntheticSwingHilOperation &) = delete;
  SyntheticSwingHilOperation(SyntheticSwingHilOperation &&) = delete;
  SyntheticSwingHilOperation &operator=(SyntheticSwingHilOperation &&) = delete;

  void Start();
  [[nodiscard]] SyntheticSwingHilOperationStatus Status() const;

 private:
  void Run(const std::stop_token &stop_token) noexcept;
  void SetStage(SyntheticSwingHilStage stage, const SyntheticSwingCaptureStatus &capture = {});
  [[nodiscard]] SyntheticSwingCaptureStatus WaitForArmed(
      const std::stop_token &stop_token, std::chrono::steady_clock::time_point deadline) const;
  [[nodiscard]] SyntheticSwingCaptureStatus WaitForCompletion(
      const std::stop_token &stop_token, std::chrono::steady_clock::time_point deadline);
  void RecordSuccess(const SyntheticSwingCaptureStatus &capture);
  void RecordFailure(std::string error) noexcept;
  void StopAndJoin() noexcept;

  SyntheticSwingHilOperationConfig config_;
  SyntheticSwingHilOperationHooks hooks_;
  std::mutex start_mutex_;
  mutable std::mutex mutex_;
  SyntheticSwingHilOperationStatus status_;
  std::jthread worker_;
};

}  // namespace swing_capture::service

#endif  // SWING_CAPTURE_CAPTURE_SERVICE_SYNTHETIC_SWING_HIL_OPERATION_H_
