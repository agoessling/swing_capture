package com.agoessling.swingcapture;

public final class PoseHighSpeedTimeoutLifecycleTest {
  public static void main(String[] arguments) {
    using(false, false, false, PoseHighSpeedTimeoutLifecycle.Phase.INACTIVE);
    using(true, true, true, PoseHighSpeedTimeoutLifecycle.Phase.WAITING_FOR_IMPACT);
    using(true, true, false, PoseHighSpeedTimeoutLifecycle.Phase.COMPLETING_CAPTURE);

    check(
        PoseHighSpeedTimeoutLifecycle.action(
                PoseHighSpeedTimeoutLifecycle.Timeout.ACTIVE_EVIDENCE,
                PoseHighSpeedTimeoutLifecycle.Phase.WAITING_FOR_IMPACT)
            == PoseHighSpeedTimeoutLifecycle.Action.RETAIN_NO_IMPACT,
        "15-second evidence expiry retains the false arm");
    check(
        PoseHighSpeedTimeoutLifecycle.action(
                PoseHighSpeedTimeoutLifecycle.Timeout.ACTIVE_EVIDENCE,
                PoseHighSpeedTimeoutLifecycle.Phase.COMPLETING_CAPTURE)
            == PoseHighSpeedTimeoutLifecycle.Action.IGNORE,
        "accepted impact is not replaced by false-arm evidence");
    check(
        PoseHighSpeedTimeoutLifecycle.action(
                PoseHighSpeedTimeoutLifecycle.Timeout.THERMAL_HARD_CAP,
                PoseHighSpeedTimeoutLifecycle.Phase.WAITING_FOR_IMPACT)
            == PoseHighSpeedTimeoutLifecycle.Action.FAIL_THERMAL_HARD_CAP,
        "30-second cap stops an untriggered capture");
    check(
        PoseHighSpeedTimeoutLifecycle.action(
                PoseHighSpeedTimeoutLifecycle.Timeout.THERMAL_HARD_CAP,
                PoseHighSpeedTimeoutLifecycle.Phase.COMPLETING_CAPTURE)
            == PoseHighSpeedTimeoutLifecycle.Action.FAIL_THERMAL_HARD_CAP,
        "30-second cap remains absolute during a hung publication");
    check(
        PoseHighSpeedTimeoutLifecycle.action(
                PoseHighSpeedTimeoutLifecycle.Timeout.THERMAL_HARD_CAP,
                PoseHighSpeedTimeoutLifecycle.Phase.INACTIVE)
            == PoseHighSpeedTimeoutLifecycle.Action.IGNORE,
        "completed capture cancels stale callbacks");
  }

  private static void using(
      boolean attempt,
      boolean engine,
      boolean waiting,
      PoseHighSpeedTimeoutLifecycle.Phase expected) {
    check(PoseHighSpeedTimeoutLifecycle.phase(attempt, engine, waiting) == expected, "phase");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
