package com.agoessling.swingcapture;

/** Regression for coincident standby-impact evidence and pose arming. */
public final class PoseTransitionPrecedenceTest {
  private PoseTransitionPrecedenceTest() {}

  public static void main(String[] arguments) {
    check(
        PoseTransitionPrecedence.resolve(false, true)
            == PoseTransitionPrecedence.Outcome.DIAGNOSTIC_WINS,
        "confirmed diagnostic before pose claim must win");
    check(
        PoseTransitionPrecedence.resolve(false, false)
            == PoseTransitionPrecedence.Outcome.POSE_WINS,
        "idle diagnostic path must permit pose claim");
    check(
        PoseTransitionPrecedence.resolve(true, true)
            == PoseTransitionPrecedence.Outcome.POSE_WINS_AND_CANCELS_DIAGNOSTIC,
        "diagnostic racing after pose claim must be truncated");
    check(
        PoseTransitionPrecedence.resolve(true, false)
            == PoseTransitionPrecedence.Outcome.POSE_WINS,
        "claimed pose remains authoritative");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
