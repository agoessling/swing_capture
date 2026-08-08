#include "capture/session/capture_session_coordinator.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>

#include "capture/trigger/impact_detector.h"

namespace swing_capture {
namespace {

using Clock = std::chrono::steady_clock;

void ValidateConfig(const CaptureSessionConfig &config) {
  if (config.pre_roll < Clock::duration::zero()) {
    throw std::invalid_argument("pre_roll cannot be negative");
  }
  if (config.post_roll < Clock::duration::zero()) {
    throw std::invalid_argument("post_roll cannot be negative");
  }
  if (config.frame_boundary_margin < Clock::duration::zero()) {
    throw std::invalid_argument("frame_boundary_margin cannot be negative");
  }
  if (config.active_ring_retention < Clock::duration::zero()) {
    throw std::invalid_argument("active_ring_retention cannot be negative");
  }
  if (config.cooldown_after_freeze < Clock::duration::zero()) {
    throw std::invalid_argument("cooldown_after_freeze cannot be negative");
  }

  // Written incrementally so an extreme invalid configuration cannot overflow
  // the duration representation during validation.
  if (config.pre_roll > config.active_ring_retention ||
      config.post_roll > config.active_ring_retention - config.pre_roll ||
      config.frame_boundary_margin >
          config.active_ring_retention - config.pre_roll - config.post_roll ||
      config.frame_boundary_margin > config.active_ring_retention - config.pre_roll -
                                         config.post_roll - config.frame_boundary_margin) {
    throw std::invalid_argument(
        "active_ring_retention must be at least pre_roll + post_roll + "
        "2 * frame_boundary_margin");
  }
}

}  // namespace

CaptureSessionCoordinator::CaptureSessionCoordinator(CaptureSessionConfig config)
    : config_(config) {
  ValidateConfig(config_);
}

ImpactHandlingResult CaptureSessionCoordinator::HandleImpact(const ImpactEvent &impact) {
  if (impact.confirmation_time < impact.strike_time) {
    throw std::invalid_argument("impact confirmation_time cannot precede strike_time");
  }
  ObserveTime(impact.confirmation_time);
  FinishCooldownIfElapsed(impact.confirmation_time);

  if (state_ == CaptureSessionState::kWaitingForPostRoll) {
    return {
        .disposition = ImpactDisposition::kIgnoredWhileClipPending,
        .related_clip_id = pending_clip_id_,
    };
  }
  if (state_ == CaptureSessionState::kCooldown) {
    return {
        .disposition = ImpactDisposition::kIgnoredDuringCooldown,
        .related_clip_id = last_frozen_clip_id_,
    };
  }

  const Clock::time_point desired_start = impact.strike_time - config_.pre_roll;
  const Clock::duration usable_retention =
      config_.active_ring_retention - config_.frame_boundary_margin;
  if (impact.confirmation_time - desired_start > usable_retention) {
    return {
        .disposition = ImpactDisposition::kRejectedPreRollUnavailable,
        .related_clip_id = 0,
    };
  }

  pending_clip_id_ = next_clip_id_++;
  pending_strike_time_ = impact.strike_time;
  pending_confirmation_time_ = impact.confirmation_time;
  freeze_deadline_ = impact.strike_time + config_.post_roll + config_.frame_boundary_margin;
  state_ = CaptureSessionState::kWaitingForPostRoll;
  return {
      .disposition = ImpactDisposition::kAccepted,
      .related_clip_id = pending_clip_id_,
  };
}

CaptureSessionTickResult CaptureSessionCoordinator::Tick(Clock::time_point now) {
  ObserveTime(now);
  FinishCooldownIfElapsed(now);
  if (state_ != CaptureSessionState::kWaitingForPostRoll) {
    return {
        .disposition = TickDisposition::kNoClipPending,
        .related_clip_id = 0,
        .freeze_request = std::nullopt,
    };
  }
  if (now < freeze_deadline_) {
    return {
        .disposition = TickDisposition::kWaitingForFreezeDeadline,
        .related_clip_id = pending_clip_id_,
        .freeze_request = std::nullopt,
    };
  }

  const std::uint64_t clip_id = pending_clip_id_;
  const Clock::time_point desired_start = pending_strike_time_ - config_.pre_roll;
  const Clock::duration usable_retention =
      config_.active_ring_retention - config_.frame_boundary_margin;
  if (now - desired_start > usable_retention) {
    pending_clip_id_ = 0;
    state_ = CaptureSessionState::kReady;
    return {
        .disposition = TickDisposition::kPreRollRetentionExpired,
        .related_clip_id = clip_id,
        .freeze_request = std::nullopt,
    };
  }

  const ClipFreezeRequest request{
      .clip_id = clip_id,
      .strike_time = pending_strike_time_,
      .impact_confirmation_time = pending_confirmation_time_,
      .desired_start_time = pending_strike_time_ - config_.pre_roll,
      .desired_end_time = pending_strike_time_ + config_.post_roll,
  };

  last_frozen_clip_id_ = clip_id;
  pending_clip_id_ = 0;
  if (config_.cooldown_after_freeze == Clock::duration::zero()) {
    state_ = CaptureSessionState::kReady;
  } else {
    cooldown_until_ = now + config_.cooldown_after_freeze;
    state_ = CaptureSessionState::kCooldown;
  }
  return {
      .disposition = TickDisposition::kFreezeRequested,
      .related_clip_id = clip_id,
      .freeze_request = request,
  };
}

CaptureSessionState CaptureSessionCoordinator::state() const { return state_; }

std::optional<std::uint64_t> CaptureSessionCoordinator::pending_clip_id() const {
  if (state_ != CaptureSessionState::kWaitingForPostRoll) {
    return std::nullopt;
  }
  return pending_clip_id_;
}

std::optional<Clock::time_point> CaptureSessionCoordinator::freeze_deadline() const {
  if (state_ != CaptureSessionState::kWaitingForPostRoll) {
    return std::nullopt;
  }
  return freeze_deadline_;
}

std::optional<Clock::time_point> CaptureSessionCoordinator::cooldown_until() const {
  if (state_ != CaptureSessionState::kCooldown) {
    return std::nullopt;
  }
  return cooldown_until_;
}

void CaptureSessionCoordinator::ObserveTime(Clock::time_point time) {
  if (has_observed_time_ && time < last_observed_time_) {
    throw std::invalid_argument("session times must be monotonically nondecreasing");
  }
  has_observed_time_ = true;
  last_observed_time_ = time;
}

void CaptureSessionCoordinator::FinishCooldownIfElapsed(Clock::time_point now) {
  if (state_ == CaptureSessionState::kCooldown && now >= cooldown_until_) {
    state_ = CaptureSessionState::kReady;
  }
}

}  // namespace swing_capture
