package com.agoessling.swingcapture;

/** Deterministic actions for the two independent production high-speed deadlines. */
final class PoseHighSpeedTimeoutLifecycle {
  enum Phase {
    INACTIVE,
    WAITING_FOR_IMPACT,
    COMPLETING_CAPTURE
  }

  enum Timeout {
    ACTIVE_EVIDENCE,
    THERMAL_HARD_CAP
  }

  enum Action {
    IGNORE,
    RETAIN_NO_IMPACT,
    FAIL_THERMAL_HARD_CAP
  }

  private PoseHighSpeedTimeoutLifecycle() {}

  static Phase phase(boolean poseHighSpeedAttempt, boolean enginePresent, boolean waitingForImpact) {
    if (!poseHighSpeedAttempt || !enginePresent) {
      return Phase.INACTIVE;
    }
    return waitingForImpact ? Phase.WAITING_FOR_IMPACT : Phase.COMPLETING_CAPTURE;
  }

  static Action action(Timeout timeout, Phase phase) {
    if (phase == Phase.INACTIVE) {
      return Action.IGNORE;
    }
    if (timeout == Timeout.THERMAL_HARD_CAP) {
      return Action.FAIL_THERMAL_HARD_CAP;
    }
    return phase == Phase.WAITING_FOR_IMPACT ? Action.RETAIN_NO_IMPACT : Action.IGNORE;
  }
}
