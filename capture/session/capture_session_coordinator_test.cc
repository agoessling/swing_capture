#include "capture/session/capture_session_coordinator.h"

#include <cassert>
#include <chrono>
#include <stdexcept>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using swing_capture::CaptureSessionConfig;
using swing_capture::CaptureSessionCoordinator;
using swing_capture::CaptureSessionState;
using swing_capture::ImpactDisposition;
using swing_capture::ImpactEvent;
using swing_capture::TickDisposition;

CaptureSessionConfig Config() {
  return {
      .pre_roll = 1s,
      .post_roll = 400ms,
      .frame_boundary_margin = 5ms,
      .active_ring_retention = 1410ms,
      .cooldown_after_freeze = 500ms,
  };
}

ImpactEvent ImpactAt(Clock::time_point strike_time, Clock::time_point confirmation_time) {
  ImpactEvent impact;
  impact.strike_time = strike_time;
  impact.confirmation_time = confirmation_time;
  return impact;
}

ImpactEvent ImpactAt(Clock::time_point strike_time) { return ImpactAt(strike_time, strike_time); }

void EmitsAtExactPostRollPlusMarginDeadline() {
  CaptureSessionCoordinator coordinator(Config());
  const auto strike = Clock::time_point(10s);

  const auto handling = coordinator.HandleImpact(ImpactAt(strike));
  assert(handling.accepted());
  assert(handling.related_clip_id == 1);
  assert(coordinator.state() == CaptureSessionState::kWaitingForPostRoll);
  assert(coordinator.freeze_deadline() == strike + 405ms);

  const auto result = coordinator.Tick(strike + 405ms);
  assert(result.disposition == TickDisposition::kFreezeRequested);
  assert(result.freeze_requested());
  assert(result.related_clip_id == 1);
  assert(result.freeze_request.has_value());
  assert(result.freeze_request->clip_id == 1);
  assert(result.freeze_request->strike_time == strike);
  assert(result.freeze_request->impact_confirmation_time == strike);
  assert(result.freeze_request->desired_start_time == strike - 1s);
  assert(result.freeze_request->desired_end_time == strike + 400ms);
  assert(coordinator.state() == CaptureSessionState::kCooldown);
}

void DoesNotEmitJustBeforeDeadline() {
  CaptureSessionCoordinator coordinator(Config());
  const auto strike = Clock::time_point(20s);
  assert(coordinator.HandleImpact(ImpactAt(strike)).accepted());

  const auto waiting = coordinator.Tick(strike + 405ms - 1ns);
  assert(waiting.disposition == TickDisposition::kWaitingForFreezeDeadline);
  assert(waiting.related_clip_id == 1);
  assert(!waiting.freeze_request.has_value());
  assert(coordinator.pending_clip_id() == 1);

  const auto result = coordinator.Tick(strike + 405ms);
  assert(result.freeze_requested());
  assert(result.freeze_request->clip_id == 1);
}

void TickAtExactRetentionBoundaryStillFreezes() {
  CaptureSessionConfig config = Config();
  config.active_ring_retention = 1420ms;
  CaptureSessionCoordinator coordinator(config);
  const auto strike = Clock::time_point(30s);
  assert(coordinator.HandleImpact(ImpactAt(strike)).accepted());

  // desired_start + retention - leading margin == strike + 415ms.
  const auto last_possible_freeze = strike + 415ms;
  const auto result = coordinator.Tick(last_possible_freeze);
  assert(result.freeze_requested());
  assert(result.freeze_request->strike_time == strike);
  assert(result.freeze_request->desired_start_time == strike - 1s);
  assert(result.freeze_request->desired_end_time == strike + 400ms);
  assert(coordinator.cooldown_until() == last_possible_freeze + 500ms);
}

void TickAfterRetentionBoundaryExplicitlyAbandonsClip() {
  CaptureSessionCoordinator coordinator(Config());
  const auto strike = Clock::time_point(35s);
  assert(coordinator.HandleImpact(ImpactAt(strike)).accepted());

  // Exact-fit retention leaves no lateness beyond the normal deadline.
  const auto result = coordinator.Tick(strike + 405ms + 1ns);
  assert(result.disposition == TickDisposition::kPreRollRetentionExpired);
  assert(!result.freeze_requested());
  assert(result.related_clip_id == 1);
  assert(!result.freeze_request.has_value());
  assert(coordinator.state() == CaptureSessionState::kReady);
  assert(!coordinator.pending_clip_id().has_value());

  const auto no_pending = coordinator.Tick(strike + 406ms);
  assert(no_pending.disposition == TickDisposition::kNoClipPending);
  assert(no_pending.related_clip_id == 0);
}

void ExplicitlyIgnoresDuplicateImpactsWhilePending() {
  CaptureSessionCoordinator coordinator(Config());
  const auto first_strike = Clock::time_point(40s);
  const auto duplicate_strike = first_strike - 20ms;
  assert(coordinator.HandleImpact(ImpactAt(first_strike)).accepted());

  // Delivery order is based on confirmation time. A later-confirmed duplicate
  // may legitimately refer to an earlier peak sample.
  const auto duplicate = coordinator.HandleImpact(ImpactAt(duplicate_strike, first_strike + 50ms));
  assert(!duplicate.accepted());
  assert(duplicate.disposition == ImpactDisposition::kIgnoredWhileClipPending);
  assert(duplicate.related_clip_id == 1);

  const auto result = coordinator.Tick(first_strike + 405ms);
  assert(result.freeze_requested());
  assert(result.freeze_request->strike_time == first_strike);
  assert(result.freeze_request->desired_end_time == first_strike + 400ms);
}

void IgnoresDuringCooldownAndAcceptsAtExactEnd() {
  CaptureSessionCoordinator coordinator(Config());
  const auto strike = Clock::time_point(50s);
  assert(coordinator.HandleImpact(ImpactAt(strike)).accepted());
  const auto freeze_time = strike + 405ms;
  assert(coordinator.Tick(freeze_time).freeze_requested());

  const auto ignored =
      coordinator.HandleImpact(ImpactAt(freeze_time - 100ms, freeze_time + 500ms - 1ns));
  assert(ignored.disposition == ImpactDisposition::kIgnoredDuringCooldown);
  assert(ignored.related_clip_id == 1);

  // The peak is backdated, but confirmation arrives exactly when cooldown
  // ends, so the new event is accepted.
  const auto accepted =
      coordinator.HandleImpact(ImpactAt(freeze_time + 400ms, freeze_time + 500ms));
  assert(accepted.accepted());
  assert(accepted.related_clip_id == 2);
}

void RejectsInvalidAndAlreadyExpiredImpacts() {
  CaptureSessionCoordinator coordinator(Config());
  const auto strike = Clock::time_point(55s);

  bool invalid_rejected = false;
  try {
    (void)coordinator.HandleImpact(ImpactAt(strike, strike - 1ns));
  } catch (const std::invalid_argument &) {
    invalid_rejected = true;
  }
  assert(invalid_rejected);
  assert(coordinator.state() == CaptureSessionState::kReady);

  // Retention equality is valid: the desired start is still the oldest
  // retained instant, and an immediate Tick can freeze it.
  const auto exact_confirmation = strike + 405ms;
  const auto exact = coordinator.HandleImpact(ImpactAt(strike, exact_confirmation));
  assert(exact.accepted());
  assert(coordinator.Tick(exact_confirmation).freeze_requested());

  CaptureSessionCoordinator late_coordinator(Config());
  const auto expired = late_coordinator.HandleImpact(ImpactAt(strike, strike + 405ms + 1ns));
  assert(expired.disposition == ImpactDisposition::kRejectedPreRollUnavailable);
  assert(!expired.accepted());
  assert(expired.related_clip_id == 0);
  assert(late_coordinator.state() == CaptureSessionState::kReady);
  assert(!late_coordinator.pending_clip_id().has_value());
}

void RejectsInvalidConfigurations() {
  const auto rejected = [](CaptureSessionConfig config) {
    try {
      CaptureSessionCoordinator coordinator(config);
      (void)coordinator;
      return false;
    } catch (const std::invalid_argument &) {
      return true;
    }
  };

  CaptureSessionConfig config = Config();
  config.pre_roll = -1ns;
  assert(rejected(config));
  config = Config();
  config.post_roll = -1ns;
  assert(rejected(config));
  config = Config();
  config.frame_boundary_margin = -1ns;
  assert(rejected(config));
  config = Config();
  config.cooldown_after_freeze = -1ns;
  assert(rejected(config));
  config = Config();
  config.active_ring_retention = -1ns;
  assert(rejected(config));
  config = Config();
  config.active_ring_retention = 1409ms;
  assert(rejected(config));

  // Equality is intentional: one boundary margin remains between the normal
  // freeze deadline and the instant requested pre-roll expires.
  CaptureSessionCoordinator exact_fit(Config());
  assert(exact_fit.state() == CaptureSessionState::kReady);
}

void RejectsNonmonotonicCallsWithoutChangingState() {
  CaptureSessionCoordinator coordinator(Config());
  const auto strike = Clock::time_point(60s);
  assert(coordinator.HandleImpact(ImpactAt(strike)).accepted());

  bool tick_rejected = false;
  try {
    (void)coordinator.Tick(strike - 1ns);
  } catch (const std::invalid_argument &) {
    tick_rejected = true;
  }
  assert(tick_rejected);
  assert(coordinator.pending_clip_id() == 1);

  assert(coordinator.Tick(strike + 100ms).disposition ==
         TickDisposition::kWaitingForFreezeDeadline);
  bool impact_rejected = false;
  try {
    (void)coordinator.HandleImpact(ImpactAt(strike + 25ms, strike + 50ms));
  } catch (const std::invalid_argument &) {
    impact_rejected = true;
  }
  assert(impact_rejected);
  assert(coordinator.pending_clip_id() == 1);

  assert(coordinator.Tick(strike + 405ms).freeze_requested());
}

void ProducesSequentialSwingIdsAndWindows() {
  CaptureSessionConfig config = Config();
  config.cooldown_after_freeze = 100ms;
  CaptureSessionCoordinator coordinator(config);
  const auto first_strike = Clock::time_point(70s);
  assert(coordinator.HandleImpact(ImpactAt(first_strike)).accepted());
  const auto first = coordinator.Tick(first_strike + 405ms);
  assert(first.freeze_requested());
  assert(first.freeze_request->clip_id == 1);

  const auto second_strike = first_strike + 505ms;
  const auto second_handling = coordinator.HandleImpact(ImpactAt(second_strike));
  assert(second_handling.accepted());
  assert(second_handling.related_clip_id == 2);
  const auto second = coordinator.Tick(second_strike + 405ms);
  assert(second.freeze_requested());
  assert(second.freeze_request->clip_id == 2);
  assert(second.freeze_request->strike_time == second_strike);
  assert(second.freeze_request->desired_start_time == second_strike - 1s);
  assert(second.freeze_request->desired_end_time == second_strike + 400ms);
  assert(coordinator.Tick(second_strike + 405ms).disposition == TickDisposition::kNoClipPending);
}

}  // namespace

int main() {
  EmitsAtExactPostRollPlusMarginDeadline();
  DoesNotEmitJustBeforeDeadline();
  TickAtExactRetentionBoundaryStillFreezes();
  TickAfterRetentionBoundaryExplicitlyAbandonsClip();
  ExplicitlyIgnoresDuplicateImpactsWhilePending();
  IgnoresDuringCooldownAndAcceptsAtExactEnd();
  RejectsInvalidAndAlreadyExpiredImpacts();
  RejectsInvalidConfigurations();
  RejectsNonmonotonicCallsWithoutChangingState();
  ProducesSequentialSwingIdsAndWindows();
  return 0;
}
