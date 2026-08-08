#ifndef SWING_CAPTURE_CAPTURE_SESSION_CAPTURE_SESSION_COORDINATOR_H_
#define SWING_CAPTURE_CAPTURE_SESSION_CAPTURE_SESSION_COORDINATOR_H_

#include <chrono>
#include <cstdint>
#include <optional>

#include "capture/trigger/impact_detector.h"

namespace swing_capture {

struct CaptureSessionConfig {
  std::chrono::steady_clock::duration pre_roll = std::chrono::seconds(1);
  std::chrono::steady_clock::duration post_roll = std::chrono::milliseconds(500);

  // Allows the first free-running frame after the desired endpoint to arrive
  // before the rings are frozen. Retention validation reserves the same
  // margin at the beginning of the requested window for scheduler jitter.
  std::chrono::steady_clock::duration frame_boundary_margin = std::chrono::milliseconds(5);

  // Duration retained by every active camera ring. At the instant post-roll
  // plus the boundary margin completes, the ring must still have a second
  // boundary margin of pre-roll headroom. That leading margin accounts for
  // the fact that N frames covering a nominal retention duration span about
  // one frame period less than that duration.
  std::chrono::steady_clock::duration active_ring_retention = std::chrono::seconds(2);

  // Starts when the freeze request is emitted, including when Tick is late.
  std::chrono::steady_clock::duration cooldown_after_freeze = std::chrono::milliseconds(500);
};

enum class CaptureSessionState {
  kReady,
  kWaitingForPostRoll,
  kCooldown,
};

enum class ImpactDisposition {
  kAccepted,

  // All further detector events are ignored until the accepted clip has
  // emitted its one freeze request. This deliberately absorbs duplicate
  // transients even if they escape the audio detector's own refractory
  // period.
  kIgnoredWhileClipPending,

  kIgnoredDuringCooldown,

  // The event was delivered after its requested pre-roll had already fallen
  // out of the active camera rings, including the leading boundary margin.
  kRejectedPreRollUnavailable,
};

struct ImpactHandlingResult {
  ImpactDisposition disposition = ImpactDisposition::kAccepted;

  // The newly allocated clip for kAccepted, the active clip for
  // kIgnoredWhileClipPending, or the most recently frozen clip for
  // kIgnoredDuringCooldown.
  std::uint64_t related_clip_id = 0;

  [[nodiscard]] bool accepted() const { return disposition == ImpactDisposition::kAccepted; }
};

struct ClipFreezeRequest {
  std::uint64_t clip_id = 0;
  std::chrono::steady_clock::time_point strike_time;
  std::chrono::steady_clock::time_point impact_confirmation_time;
  std::chrono::steady_clock::time_point desired_start_time;
  std::chrono::steady_clock::time_point desired_end_time;
};

enum class TickDisposition {
  kNoClipPending,
  kWaitingForFreezeDeadline,
  kFreezeRequested,

  // Tick arrived so late that the requested pre-roll plus its leading
  // boundary margin is no longer present. The pending clip is abandoned and
  // the coordinator returns to kReady.
  kPreRollRetentionExpired,
};

struct CaptureSessionTickResult {
  TickDisposition disposition = TickDisposition::kNoClipPending;
  std::uint64_t related_clip_id = 0;
  std::optional<ClipFreezeRequest> freeze_request;

  [[nodiscard]] bool freeze_requested() const {
    return disposition == TickDisposition::kFreezeRequested;
  }
};

// Pure state machine coordinating impact events with a continuously recording
// ring. It owns no camera, audio, or image payload resources.
//
// HandleImpact and Tick calls must have nondecreasing host steady-clock times.
// Impact calls are ordered by confirmation_time rather than the backdated
// strike_time. Equal timestamps are allowed so an impact and an immediate
// Tick can be processed deterministically. A decreasing timestamp throws
// std::invalid_argument without changing state.
class CaptureSessionCoordinator final {
 public:
  explicit CaptureSessionCoordinator(CaptureSessionConfig config);

  [[nodiscard]] ImpactHandlingResult HandleImpact(const ImpactEvent &impact);

  // Requests a freeze exactly once when now reaches or passes the pending
  // strike's post-roll plus frame-boundary deadline. A late Tick explicitly
  // reports when the requested pre-roll has expired instead of asking callers
  // to freeze an impossible window.
  [[nodiscard]] CaptureSessionTickResult Tick(std::chrono::steady_clock::time_point now);

  [[nodiscard]] CaptureSessionState state() const;
  [[nodiscard]] std::optional<std::uint64_t> pending_clip_id() const;
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> freeze_deadline() const;
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> cooldown_until() const;

 private:
  using Clock = std::chrono::steady_clock;

  void ObserveTime(Clock::time_point time);
  void FinishCooldownIfElapsed(Clock::time_point now);

  CaptureSessionConfig config_;
  CaptureSessionState state_ = CaptureSessionState::kReady;
  bool has_observed_time_ = false;
  Clock::time_point last_observed_time_;

  std::uint64_t next_clip_id_ = 1;
  std::uint64_t pending_clip_id_ = 0;
  std::uint64_t last_frozen_clip_id_ = 0;
  Clock::time_point pending_strike_time_;
  Clock::time_point pending_confirmation_time_;
  Clock::time_point freeze_deadline_;
  Clock::time_point cooldown_until_;
};

}  // namespace swing_capture

#endif  // SWING_CAPTURE_CAPTURE_SESSION_CAPTURE_SESSION_COORDINATOR_H_
