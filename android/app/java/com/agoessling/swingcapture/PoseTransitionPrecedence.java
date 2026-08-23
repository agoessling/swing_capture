package com.agoessling.swingcapture;

/** Deterministic precedence between retained standby diagnostics and a pose capture transition. */
final class PoseTransitionPrecedence {
  enum Outcome {
    DIAGNOSTIC_WINS,
    POSE_WINS,
    POSE_WINS_AND_CANCELS_DIAGNOSTIC
  }

  private PoseTransitionPrecedence() {}

  /**
   * A real pose capture always outranks standby diagnostics. Any diagnostic post-roll already in
   * progress is truncated so it cannot prevent either phone from recording the backswing.
   */
  static Outcome resolve(boolean poseClaimed, boolean diagnosticBusy) {
    if (!poseClaimed && diagnosticBusy) {
      return Outcome.POSE_WINS_AND_CANCELS_DIAGNOSTIC;
    }
    return diagnosticBusy ? Outcome.POSE_WINS_AND_CANCELS_DIAGNOSTIC : Outcome.POSE_WINS;
  }
}
