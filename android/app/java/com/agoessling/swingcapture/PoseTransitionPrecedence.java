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
   * A diagnostic that was confirmed before the pose claim is retained. Once pose owns the camera,
   * newly arriving diagnostics are truncated so a two-second post-roll cannot miss the backswing.
   */
  static Outcome resolve(boolean poseClaimed, boolean diagnosticBusy) {
    if (!poseClaimed && diagnosticBusy) {
      return Outcome.DIAGNOSTIC_WINS;
    }
    return diagnosticBusy ? Outcome.POSE_WINS_AND_CANCELS_DIAGNOSTIC : Outcome.POSE_WINS;
  }
}
