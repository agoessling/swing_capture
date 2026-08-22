package com.agoessling.swingcapture;

import com.agoessling.swingcapture.pose.PoseTriggerController;

/** Regression for a stale Camera2 frame racing an externally requested lifecycle timestamp. */
public final class PoseExternalArmLifecycleTest {
  private PoseExternalArmLifecycleTest() {}

  public static void main(String[] arguments) {
    staleCapturedFrameCompletesBeforeExternalControllerTransition();
    strictControllerStillRejectsTheOldOrdering();
  }

  private static void strictControllerStillRejectsTheOldOrdering() {
    PoseTriggerController controller =
        new PoseTriggerController(PoseTriggerController.Config.defaultsForFiveFramesPerSecond());
    controller.observe(address(100));
    controller.externalCaptureStarted(300);
    expectIllegalArgument(
        () -> controller.observe(address(200)),
        "older captured frame after external lifecycle timestamp");
  }

  private static void staleCapturedFrameCompletesBeforeExternalControllerTransition() {
    PoseTriggerControllerLease lease = new PoseTriggerControllerLease();
    PoseTriggerController controller =
        lease.acquire(PoseTriggerController.Config.defaultsForFiveFramesPerSecond());
    controller.observe(address(100));
    PoseExternalArmLifecycle lifecycle = new PoseExternalArmLifecycle();

    expectIllegalState(
        () -> lifecycle.startController(lease, () -> 300),
        "controller transition before inference drain");
    check(
        lifecycle.state() == PoseExternalArmLifecycle.State.CLAIMED,
        "rejected early transition preserves claim");

    // This frame was captured before the external arm request but finishes inference afterward.
    // Production transfer waits for this observation before declaring inference quiescent.
    controller.observe(address(200));
    lifecycle.inferenceQuiesced();
    PoseTriggerController.Decision started = lifecycle.startController(lease, () -> 300);

    check(
        started.state() == PoseTriggerController.State.ARM_REQUESTED,
        "external capture starts after stale frame drains");
    check(started.armRequestedNs().orElseThrow() == 300, "local lifecycle timestamp preserved");
    check(
        lifecycle.state() == PoseExternalArmLifecycle.State.CONTROLLER_STARTED,
        "external lifecycle reaches controller-started state");
    expectIllegalState(lifecycle::inferenceQuiesced, "duplicate inference quiescence");
  }

  private static PoseTriggerController.Observation address(long timestampNs) {
    return new PoseTriggerController.Observation(timestampNs, 0.9, 0.9, 0.1, true);
  }

  private static void expectIllegalState(Runnable action, String label) {
    try {
      action.run();
      throw new AssertionError("Expected IllegalStateException: " + label);
    } catch (IllegalStateException expected) {
      // Expected.
    }
  }

  private static void expectIllegalArgument(Runnable action, String label) {
    try {
      action.run();
      throw new AssertionError("Expected IllegalArgumentException: " + label);
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
