package com.agoessling.swingcapture.pose;

/** Deterministic boundary coverage for the model-independent pose trigger controller. */
public final class PoseTriggerControllerTest {
  private static final long MS = 1_000_000L;

  private PoseTriggerControllerTest() {}

  public static void main(String[] arguments) {
    requiresThreeStableFiveFpsObservations();
    toleratesOneIsolatedDropout();
    rejectsMotionAndLowConfidence();
    observationGapRestartsQualification();
    armRequestIsOneShotAndTimesOut();
    completedCaptureRequiresCooldownAndClearRegion();
    rejectsNonIncreasingTimestamps();
  }

  private static void requiresThreeStableFiveFpsObservations() {
    PoseTriggerController controller = controller();

    check(command(controller, 0, 0.9, 0.2, 0.05, true) == none(), "non-address command");
    check(controller.state() == PoseTriggerController.State.WATCHING, "non-address state");
    check(command(controller, 200, 0.9, 0.8, 0.05, true) == none(), "candidate one");
    check(controller.state() == PoseTriggerController.State.QUALIFYING, "candidate one state");
    check(command(controller, 400, 0.9, 0.8, 0.05, true) == none(), "candidate two");
    check(
        command(controller, 600, 0.9, 0.8, 0.05, true)
            == PoseTriggerController.Command.START_HIGH_SPEED,
        "candidate three arms");
    check(controller.state() == PoseTriggerController.State.ARM_REQUESTED, "armed state");
  }

  private static void toleratesOneIsolatedDropout() {
    PoseTriggerController controller = controller();

    command(controller, 0, 0.9, 0.8, 0.05, true);
    command(controller, 200, 0.1, 0.0, 0.0, false);
    check(controller.state() == PoseTriggerController.State.QUALIFYING, "dropout grace state");
    check(
        command(controller, 400, 0.9, 0.8, 0.05, true)
            == PoseTriggerController.Command.START_HIGH_SPEED,
        "dropout grace arm");
  }

  private static void rejectsMotionAndLowConfidence() {
    PoseTriggerController controller = controller();

    command(controller, 0, 0.9, 0.9, 0.5, true);
    command(controller, 200, 0.4, 0.9, 0.0, true);
    command(controller, 400, 0.9, 0.4, 0.0, true);
    command(controller, 600, 0.9, 0.9, 0.0, false);

    check(controller.state() == PoseTriggerController.State.WATCHING, "rejection state");
  }

  private static void observationGapRestartsQualification() {
    PoseTriggerController controller = controller();

    command(controller, 0, 0.9, 0.9, 0.0, true);
    PoseTriggerController.Decision restarted =
        controller.observe(
            new PoseTriggerController.Observation(600 * MS, 0.9, 0.9, 0.0, true));
    check(restarted.command() == none(), "gap restart command");
    check(controller.state() == PoseTriggerController.State.QUALIFYING, "gap restart state");
    check(
        restarted.qualificationStartedNs().orElseThrow() == 600 * MS,
        "gap restart timestamp");
  }

  private static void armRequestIsOneShotAndTimesOut() {
    PoseTriggerController controller = controller();
    command(controller, 0, 0.9, 0.9, 0.0, true);
    command(controller, 200, 0.9, 0.9, 0.0, true);
    check(
        command(controller, 400, 0.9, 0.9, 0.0, true)
            == PoseTriggerController.Command.START_HIGH_SPEED,
        "initial arm command");
    check(command(controller, 600, 0.9, 0.9, 0.0, true) == none(), "one-shot command");
    check(
        command(controller, 15_400, 0.9, 0.9, 0.0, true)
            == PoseTriggerController.Command.STOP_HIGH_SPEED,
        "arm timeout command");
    check(
        controller.state() == PoseTriggerController.State.WAITING_FOR_CLEAR,
        "arm timeout state");
  }

  private static void completedCaptureRequiresCooldownAndClearRegion() {
    PoseTriggerController controller = controller();
    command(controller, 0, 0.9, 0.9, 0.0, true);
    command(controller, 200, 0.9, 0.9, 0.0, true);
    command(controller, 400, 0.9, 0.9, 0.0, true);
    controller.captureEnded(500 * MS);

    command(controller, 700, 0.0, 0.0, 0.0, false);
    command(controller, 1_900, 0.0, 0.0, 0.0, false);
    check(
        controller.state() == PoseTriggerController.State.WAITING_FOR_CLEAR,
        "cooldown still active");
    command(controller, 2_500, 0.0, 0.0, 0.0, false);
    check(controller.state() == PoseTriggerController.State.WATCHING, "cooldown and clear done");
  }

  private static void rejectsNonIncreasingTimestamps() {
    PoseTriggerController controller = controller();
    command(controller, 100, 0.0, 0.0, 0.0, false);
    expectThrows(
        IllegalArgumentException.class,
        () -> command(controller, 100, 0.0, 0.0, 0.0, false),
        "duplicate timestamp");
    expectThrows(
        IllegalArgumentException.class,
        () -> command(controller, 99, 0.0, 0.0, 0.0, false),
        "decreasing timestamp");
  }

  private static PoseTriggerController controller() {
    return new PoseTriggerController(PoseTriggerController.Config.defaultsForFiveFramesPerSecond());
  }

  private static PoseTriggerController.Command command(
      PoseTriggerController controller,
      long timestampMs,
      double person,
      double address,
      double motion,
      boolean insideRegion) {
    return controller
        .observe(
            new PoseTriggerController.Observation(
                timestampMs * MS, person, address, motion, insideRegion))
        .command();
  }

  private static PoseTriggerController.Command none() {
    return PoseTriggerController.Command.NONE;
  }

  private static <T extends Throwable> void expectThrows(
      Class<T> expected, Runnable action, String message) {
    try {
      action.run();
    } catch (Throwable throwable) {
      if (expected.isInstance(throwable)) {
        return;
      }
      throw new AssertionError(message + " threw " + throwable, throwable);
    }
    throw new AssertionError(message + " did not throw " + expected.getSimpleName());
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
