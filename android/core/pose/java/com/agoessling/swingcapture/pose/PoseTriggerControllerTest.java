package com.agoessling.swingcapture.pose;

/** Deterministic boundary and off-nominal coverage for the pose trigger controller. */
public final class PoseTriggerControllerTest {
  private static final long MS = 1_000_000L;

  private PoseTriggerControllerTest() {}

  public static void main(String[] arguments) {
    requiresThreeStableFiveFpsObservations();
    toleratesOneIsolatedDropout();
    rejectsMotionAndLowConfidence();
    abortedAddressReturnsToWatchingAndLaterSetupArms();
    emptySceneAndWalkThroughNeverQualify();
    doesNotRequireAConfiguredHittingRegion();
    observationGapRestartsQualification();
    addressDwellSlidesPastFifteenSecondsUntilThermalCap();
    falseArmAndPracticeSwingRemainReadyForRealSetup();
    clearStepAwayStopsAndRequiresCooldownBeforeReentry();
    completedCaptureRequiresCooldownAndResetEvidence();
    postShotFinishCannotRearmWithoutResetEvidence();
    completedResetCanPrecedeTheNextAddress();
    completedCaptureRearmsAfterSwingWithoutLeavingFrame();
    externalCaptureTransitionsWatchingAndQualifying();
    externalCaptureRefreshesLocalShadowArm();
    externalCaptureCannotBypassClearLatch();
    clearEvidenceDoesNotSpanInferenceGap();
    inferenceGapCannotExtendExpiredEvidenceRetroactively();
    legacyConfigRetainsFixedArmedLimit();
    rejectsNonIncreasingTimestamps();
  }

  private static void requiresThreeStableFiveFpsObservations() {
    PoseTriggerController controller = controller();

    check(decision(controller, 0, 0.9, 0.2, 0.05, true).command() == none(), "non-address");
    check(controller.state() == PoseTriggerController.State.WATCHING, "non-address state");
    PoseTriggerController.Decision first = decision(controller, 200, 0.9, 0.8, 0.05, true);
    check(first.command() == none(), "candidate one");
    check(
        first.transitionReason() == PoseTriggerController.TransitionReason.QUALIFICATION_STARTED,
        "candidate reason");
    check(controller.state() == PoseTriggerController.State.QUALIFYING, "candidate one state");
    check(decision(controller, 400, 0.9, 0.8, 0.05, true).command() == none(), "candidate two");
    PoseTriggerController.Decision armed = decision(controller, 600, 0.9, 0.8, 0.05, true);
    check(armed.command() == PoseTriggerController.Command.START_HIGH_SPEED, "candidate three");
    check(
        armed.transitionReason() == PoseTriggerController.TransitionReason.ADDRESS_STABLE_ARMED,
        "arm reason");
    check(armed.activeUntilNs().orElseThrow() == 15_600 * MS, "initial active lease");
    check(armed.thermalHardStopNs().orElseThrow() == 30_600 * MS, "thermal deadline");
    check(armed.qualificationStartedNs().isEmpty(), "qualification closes on arm");
    check(controller.state() == PoseTriggerController.State.ARM_REQUESTED, "armed state");
  }

  private static void toleratesOneIsolatedDropout() {
    PoseTriggerController controller = controller();

    decision(controller, 0, 0.9, 0.8, 0.05, true);
    PoseTriggerController.Decision dropout = decision(controller, 200, 0.1, 0.0, 0.0, false);
    check(controller.state() == PoseTriggerController.State.QUALIFYING, "dropout grace state");
    check(
        dropout.transitionReason()
            == PoseTriggerController.TransitionReason.QUALIFICATION_DROPOUT_TOLERATED,
        "dropout reason");
    check(
        decision(controller, 400, 0.9, 0.8, 0.05, true).command()
            == PoseTriggerController.Command.START_HIGH_SPEED,
        "dropout grace arm");
  }

  private static void rejectsMotionAndLowConfidence() {
    PoseTriggerController controller = controller();

    decision(controller, 0, 0.9, 0.9, 0.5, true);
    decision(controller, 200, 0.4, 0.9, 0.0, true);
    decision(controller, 400, 0.9, 0.4, 0.0, true);
    check(controller.state() == PoseTriggerController.State.WATCHING, "rejection state");
  }

  private static void abortedAddressReturnsToWatchingAndLaterSetupArms() {
    PoseTriggerController controller = controller();

    decision(controller, 0, 0.95, 0.9, 0.1, false);
    decision(controller, 200, 0.95, 0.9, 0.1, false);
    PoseTriggerController.Decision tolerated =
        decision(controller, 400, 0.95, 0.2, 0.9, false);
    check(
        tolerated.transitionReason()
            == PoseTriggerController.TransitionReason.QUALIFICATION_DROPOUT_TOLERATED,
        "brief aborted-address dropout is tolerated");
    PoseTriggerController.Decision aborted =
        decision(controller, 600, 0.95, 0.2, 0.9, false);
    check(
        aborted.transitionReason()
            == PoseTriggerController.TransitionReason.QUALIFICATION_DROPPED,
        "aborted address ends qualification");
    check(controller.state() == PoseTriggerController.State.WATCHING, "aborted address watches");

    decision(controller, 800, 0.95, 0.9, 0.1, false);
    decision(controller, 1_000, 0.95, 0.9, 0.1, false);
    check(
        decision(controller, 1_200, 0.95, 0.9, 0.1, false).command()
            == PoseTriggerController.Command.START_HIGH_SPEED,
        "later complete setup arms normally");
  }

  private static void emptySceneAndWalkThroughNeverQualify() {
    PoseTriggerController controller = controller();
    int startCommands = 0;

    for (long timestampMs = 0; timestampMs <= 1_000; timestampMs += 200) {
      PoseTriggerController.Decision empty =
          decision(controller, timestampMs, 0.0, 0.0, 0.0, false);
      startCommands +=
          empty.command() == PoseTriggerController.Command.START_HIGH_SPEED ? 1 : 0;
      check(
          empty.transitionReason() == PoseTriggerController.TransitionReason.WAITING_FOR_ADDRESS,
          "empty scene waits at " + timestampMs);
    }
    for (long timestampMs = 1_200; timestampMs <= 3_000; timestampMs += 200) {
      PoseTriggerController.Decision walkThrough =
          decision(controller, timestampMs, 0.95, 0.2, 0.9, false);
      startCommands +=
          walkThrough.command() == PoseTriggerController.Command.START_HIGH_SPEED ? 1 : 0;
      check(
          walkThrough.transitionReason()
              == PoseTriggerController.TransitionReason.WAITING_FOR_ADDRESS,
          "walk-through waits at " + timestampMs);
    }
    check(startCommands == 0, "empty and walk-through sequence never arms");
    check(controller.state() == PoseTriggerController.State.WATCHING, "non-address state");
  }

  private static void doesNotRequireAConfiguredHittingRegion() {
    PoseTriggerController controller = controller();

    decision(controller, 0, 0.9, 0.9, 0.0, false);
    decision(controller, 200, 0.9, 0.9, 0.0, false);
    PoseTriggerController.Decision armed = decision(controller, 400, 0.9, 0.9, 0.0, false);

    check(
        armed.command() == PoseTriggerController.Command.START_HIGH_SPEED,
        "full-frame pose can arm outside a legacy ROI");
  }

  private static void observationGapRestartsQualification() {
    PoseTriggerController controller = controller();

    decision(controller, 0, 0.9, 0.9, 0.0, true);
    PoseTriggerController.Decision restarted = decision(controller, 600, 0.9, 0.9, 0.0, true);
    check(restarted.command() == none(), "gap restart command");
    check(controller.state() == PoseTriggerController.State.QUALIFYING, "gap restart state");
    check(restarted.qualificationStartedNs().orElseThrow() == 600 * MS, "gap restart timestamp");
    check(
        restarted.transitionReason()
            == PoseTriggerController.TransitionReason.OBSERVATION_GAP_RESTARTED,
        "gap restart reason");
  }

  private static void addressDwellSlidesPastFifteenSecondsUntilThermalCap() {
    PoseTriggerController controller = armedController();
    PoseTriggerController.Decision current = null;
    for (long timestampMs = 600; timestampMs <= 30_200; timestampMs += 200) {
      current = decision(controller, timestampMs, 0.9, 0.9, 0.0, true);
      check(current.command() == none(), "dwell must stay active at " + timestampMs);
    }
    check(current != null, "dwell decision present");
    check(
        current.transitionReason() == PoseTriggerController.TransitionReason.ACTIVE_WINDOW_EXTENDED,
        "sliding extension reason");
    check(current.activeUntilNs().orElseThrow() == 30_400 * MS, "extension clamps to hard cap");

    PoseTriggerController.Decision capped = decision(controller, 30_400, 0.9, 0.9, 0.0, true);
    check(capped.command() == PoseTriggerController.Command.STOP_HIGH_SPEED, "thermal cap stop");
    check(
        capped.transitionReason()
            == PoseTriggerController.TransitionReason.THERMAL_HARD_CAP_REACHED,
        "thermal cap reason");
    check(
        controller.state() == PoseTriggerController.State.WAITING_FOR_CLEAR,
        "thermal cap latches until clear");
  }

  private static void falseArmAndPracticeSwingRemainReadyForRealSetup() {
    PoseTriggerController controller = armedController();
    int additionalStarts = 0;
    for (long timestampMs = 600; timestampMs <= 12_000; timestampMs += 200) {
      PoseTriggerController.Decision practice =
          decision(controller, timestampMs, 0.9, 0.1, 0.95, true);
      if (practice.command() == PoseTriggerController.Command.START_HIGH_SPEED) {
        additionalStarts++;
      }
      check(practice.command() != PoseTriggerController.Command.STOP_HIGH_SPEED, "practice stop");
      check(
          practice.transitionReason()
              == PoseTriggerController.TransitionReason.ACTIVE_WINDOW_EXTENDED,
          "practice swing remains engaged");
    }

    PoseTriggerController.Decision realSetup = decision(controller, 12_200, 0.9, 0.9, 0.0, true);
    check(realSetup.command() == none(), "real setup uses already-running capture");
    check(additionalStarts == 0, "arm command remains one-shot");
    check(controller.state() == PoseTriggerController.State.ARM_REQUESTED, "real setup remains armed");
  }

  private static void clearStepAwayStopsAndRequiresCooldownBeforeReentry() {
    PoseTriggerController controller = armedController();
    for (long timestampMs = 600; timestampMs < 1_600; timestampMs += 200) {
      PoseTriggerController.Decision clearing =
          decision(controller, timestampMs, 0.0, 0.0, 0.0, false);
      check(clearing.command() == none(), "clear debounce at " + timestampMs);
    }
    PoseTriggerController.Decision stopped = decision(controller, 1_600, 0.0, 0.0, 0.0, false);
    check(stopped.command() == PoseTriggerController.Command.STOP_HIGH_SPEED, "clear stops active");
    check(
        stopped.transitionReason() == PoseTriggerController.TransitionReason.ACTIVE_CLEAR_STOPPED,
        "clear stop reason");
    check(
        controller.state() == PoseTriggerController.State.WAITING_FOR_CLEAR,
        "clear stop enters cooldown");

    // Returning during cooldown must not start qualification.
    decision(controller, 1_800, 0.9, 0.9, 0.0, true);
    decision(controller, 2_000, 0.9, 0.9, 0.0, true);
    check(
        decision(controller, 2_200, 0.9, 0.9, 0.0, true).command() == none(),
        "reentry during cooldown does not rearm");
    check(
        controller.state() == PoseTriggerController.State.WAITING_FOR_CLEAR,
        "reentry remains latched until cooldown elapses");

    for (long timestampMs = 2_400; timestampMs <= 3_600; timestampMs += 200) {
      decision(controller, timestampMs, 0.0, 0.0, 0.0, false);
    }
    check(controller.state() == PoseTriggerController.State.WATCHING, "fresh clear rearms");
    decision(controller, 3_800, 0.9, 0.9, 0.0, true);
    decision(controller, 4_000, 0.9, 0.9, 0.0, true);
    check(
        decision(controller, 4_200, 0.9, 0.9, 0.0, true).command()
            == PoseTriggerController.Command.START_HIGH_SPEED,
        "post-clear setup arms normally");
  }

  private static void completedCaptureRequiresCooldownAndResetEvidence() {
    PoseTriggerController controller = armedController();
    PoseTriggerController.Decision ended = controller.captureEnded(500 * MS);
    check(
        ended.transitionReason() == PoseTriggerController.TransitionReason.CAPTURE_ENDED,
        "capture-end reason");

    for (long timestampMs = 700; timestampMs < 1_700; timestampMs += 200) {
      decision(controller, timestampMs, 0.0, 0.0, 0.0, false);
    }
    PoseTriggerController.Decision cleared = decision(controller, 1_700, 0.0, 0.0, 0.0, false);
    check(
        cleared.transitionReason() == PoseTriggerController.TransitionReason.WAITING_FOR_CLEAR,
        "clear alone cannot beat cooldown");
    check(
        controller.state() == PoseTriggerController.State.WAITING_FOR_CLEAR,
        "cooldown remains active");

    PoseTriggerController.Decision ready = cleared;
    for (long timestampMs = 1_900; timestampMs <= 2_500; timestampMs += 200) {
      ready = decision(controller, timestampMs, 0.0, 0.0, 0.0, false);
    }
    check(
        ready.transitionReason() == PoseTriggerController.TransitionReason.CLEAR_COMPLETE_REARMED,
        "completed capture cooldown reason");
    check(controller.state() == PoseTriggerController.State.WATCHING, "cooldown and clear done");
  }

  private static void postShotFinishCannotRearmWithoutResetEvidence() {
    PoseTriggerController controller = armedController();
    controller.captureEnded(500 * MS);

    for (long timestampMs = 700; timestampMs <= 2_700; timestampMs += 200) {
      PoseTriggerController.Decision finish =
          decision(controller, timestampMs, 0.95, 0.9, 0.1, false);
      check(finish.command() == none(), "finish pose cannot rearm at " + timestampMs);
      check(
          finish.transitionReason() == PoseTriggerController.TransitionReason.WAITING_FOR_CLEAR,
          "finish pose remains latched at " + timestampMs);
    }
    check(
        controller.state() == PoseTriggerController.State.WAITING_FOR_CLEAR,
        "cooldown alone cannot clear an address-like finish");

    PoseTriggerController.Decision reset =
        decision(controller, 2_900, 0.95, 0.2, 0.9, false);
    check(
        reset.transitionReason() == PoseTriggerController.TransitionReason.CLEAR_COMPLETE_REARMED,
        "post-shot motion releases the elapsed latch");
    decision(controller, 3_100, 0.95, 0.9, 0.1, false);
    decision(controller, 3_300, 0.95, 0.9, 0.1, false);
    check(
        decision(controller, 3_500, 0.95, 0.9, 0.1, false).command()
            == PoseTriggerController.Command.START_HIGH_SPEED,
        "repeated setup arms after explicit reset evidence");
  }

  private static void completedResetCanPrecedeTheNextAddress() {
    PoseTriggerController controller = armedController();
    controller.captureEnded(500 * MS);

    decision(controller, 700, 0.95, 0.2, 0.9, true);
    for (long timestampMs = 900; timestampMs < 2_500; timestampMs += 200) {
      PoseTriggerController.Decision waiting =
          decision(controller, timestampMs, 0.95, 0.9, 0.1, true);
      check(waiting.command() == none(), "new address waits for cooldown");
    }
    check(controller.state() == PoseTriggerController.State.WAITING_FOR_CLEAR, "cooldown latch");

    PoseTriggerController.Decision ready =
        decision(controller, 2_500, 0.95, 0.9, 0.1, true);
    check(
        ready.transitionReason()
            == PoseTriggerController.TransitionReason.CLEAR_COMPLETE_REARMED,
        "remembered reset releases at cooldown");
    decision(controller, 2_700, 0.95, 0.9, 0.1, true);
    decision(controller, 2_900, 0.95, 0.9, 0.1, true);
    check(
        decision(controller, 3_100, 0.95, 0.9, 0.1, true).command()
            == PoseTriggerController.Command.START_HIGH_SPEED,
        "new address qualifies after cooldown");
  }

  private static void completedCaptureRearmsAfterSwingWithoutLeavingFrame() {
    PoseTriggerController controller = armedController();
    controller.captureEnded(700 * MS);

    boolean resetObserved = false;
    for (long timestampMs = 900; timestampMs <= 2_900; timestampMs += 200) {
      PoseTriggerController.Decision decision =
          decision(controller, timestampMs, 0.95, 0.2, 0.9, true);
      resetObserved |=
          decision.transitionReason()
              == PoseTriggerController.TransitionReason.CLEAR_COMPLETE_REARMED;
    }

    check(resetObserved, "post-swing motion and non-address posture clear the latch");
    check(controller.state() == PoseTriggerController.State.WATCHING, "post-swing ready state");

    decision(controller, 3_100, 0.95, 0.9, 0.1, true);
    decision(controller, 3_300, 0.95, 0.9, 0.1, true);
    check(
        decision(controller, 3_500, 0.95, 0.9, 0.1, true).command()
            == PoseTriggerController.Command.START_HIGH_SPEED,
        "fresh address rearms without walking out of frame");
  }

  private static void externalCaptureTransitionsWatchingAndQualifying() {
    PoseTriggerController watching = controller();
    PoseTriggerController.Decision started = watching.externalCaptureStarted(100 * MS);
    check(started.state() == PoseTriggerController.State.ARM_REQUESTED, "external watching state");
    check(started.command() == none(), "external start does not emit a second start command");
    check(
        started.transitionReason()
            == PoseTriggerController.TransitionReason.EXTERNAL_CAPTURE_STARTED,
        "external watching reason");
    check(started.armRequestedNs().orElseThrow() == 100 * MS, "external watching timestamp");
    check(
        watching.captureEnded(200 * MS).state()
            == PoseTriggerController.State.WAITING_FOR_CLEAR,
        "external watching capture can end");

    PoseTriggerController qualifying = controller();
    decision(qualifying, 0, 0.9, 0.9, 0.0, true);
    PoseTriggerController.Decision qualifyingStart = qualifying.externalCaptureStarted(100 * MS);
    check(
        qualifyingStart.state() == PoseTriggerController.State.ARM_REQUESTED,
        "external qualifying state");
    check(qualifyingStart.qualificationStartedNs().isEmpty(), "external closes qualification");
  }

  private static void externalCaptureRefreshesLocalShadowArm() {
    PoseTriggerController shadow = armedController();
    PoseTriggerController.Decision refreshed = shadow.externalCaptureStarted(600 * MS);
    check(refreshed.armRequestedNs().orElseThrow() == 600 * MS, "external refresh timestamp");
    check(refreshed.activeUntilNs().orElseThrow() == 15_600 * MS, "external refresh lease");
    check(refreshed.thermalHardStopNs().orElseThrow() == 30_600 * MS, "external refresh cap");
    shadow.captureEnded(700 * MS);
  }

  private static void externalCaptureCannotBypassClearLatch() {
    PoseTriggerController controller = armedController();
    controller.captureEnded(500 * MS);
    expectThrows(
        IllegalStateException.class,
        () -> controller.externalCaptureStarted(600 * MS),
        "external clear latch");
  }

  private static void inferenceGapCannotExtendExpiredEvidenceRetroactively() {
    PoseTriggerController controller = armedController();
    PoseTriggerController.Decision practice = decision(controller, 1_000, 0.9, 0.1, 0.9, true);
    check(practice.command() == none(), "short inference gap remains active");

    PoseTriggerController.Decision expired = decision(controller, 16_000, 0.9, 0.9, 0.0, true);
    check(expired.command() == PoseTriggerController.Command.STOP_HIGH_SPEED, "stale lease stops");
    check(
        expired.transitionReason() == PoseTriggerController.TransitionReason.ACTIVE_EVIDENCE_EXPIRED,
        "stale lease reason");
    check(
        controller.state() == PoseTriggerController.State.WAITING_FOR_CLEAR,
        "recovered frame cannot immediately begin qualification");

    decision(controller, 16_200, 0.9, 0.9, 0.0, true);
    check(
        decision(controller, 16_400, 0.9, 0.9, 0.0, true).command() == none(),
        "recovered inference remains latched");
    for (long timestampMs = 16_600; timestampMs <= 18_000; timestampMs += 200) {
      decision(controller, timestampMs, 0.0, 0.0, 0.0, false);
    }
    check(controller.state() == PoseTriggerController.State.WATCHING, "clear after gap rearms");
  }

  private static void clearEvidenceDoesNotSpanInferenceGap() {
    PoseTriggerController controller = armedController();
    decision(controller, 600, 0.0, 0.0, 0.0, false);
    decision(controller, 800, 0.0, 0.0, 0.0, false);

    PoseTriggerController.Decision afterGap =
        decision(controller, 1_400, 0.0, 0.0, 0.0, false);
    check(afterGap.command() == none(), "clear gap must restart debounce");
    for (long timestampMs = 1_600; timestampMs < 2_400; timestampMs += 200) {
      check(
          decision(controller, timestampMs, 0.0, 0.0, 0.0, false).command() == none(),
          "continuous clear after inference gap at " + timestampMs);
    }
    check(
        decision(controller, 2_400, 0.0, 0.0, 0.0, false).command()
            == PoseTriggerController.Command.STOP_HIGH_SPEED,
        "clear completes only after fresh continuous evidence");
  }

  private static void legacyConfigRetainsFixedArmedLimit() {
    PoseTriggerController.Config defaults =
        PoseTriggerController.Config.defaultsForFiveFramesPerSecond();
    PoseTriggerController.Config legacy =
        new PoseTriggerController.Config(
            defaults.minimumPersonConfidence(),
            defaults.maximumClearPersonConfidence(),
            defaults.minimumAddressConfidence(),
            defaults.maximumMotionMagnitude(),
            defaults.minimumQualificationNs(),
            defaults.maximumObservationGapNs(),
            defaults.qualificationDropoutGraceNs(),
            defaults.maximumArmedDurationNs(),
            defaults.clearDurationNs(),
            defaults.cooldownNs());
    check(
        legacy.thermalHardCapNs() == legacy.maximumArmedDurationNs(),
        "legacy constructor fixed cap");
  }

  private static void rejectsNonIncreasingTimestamps() {
    PoseTriggerController controller = controller();
    decision(controller, 100, 0.0, 0.0, 0.0, false);
    expectThrows(
        IllegalArgumentException.class,
        () -> decision(controller, 100, 0.0, 0.0, 0.0, false),
        "duplicate timestamp");
    expectThrows(
        IllegalArgumentException.class,
        () -> decision(controller, 99, 0.0, 0.0, 0.0, false),
        "decreasing timestamp");
  }

  private static PoseTriggerController armedController() {
    PoseTriggerController controller = controller();
    decision(controller, 0, 0.9, 0.9, 0.0, true);
    decision(controller, 200, 0.9, 0.9, 0.0, true);
    check(
        decision(controller, 400, 0.9, 0.9, 0.0, true).command()
            == PoseTriggerController.Command.START_HIGH_SPEED,
        "test setup arm");
    return controller;
  }

  private static PoseTriggerController controller() {
    return new PoseTriggerController(PoseTriggerController.Config.defaultsForFiveFramesPerSecond());
  }

  private static PoseTriggerController.Decision decision(
      PoseTriggerController controller,
      long timestampMs,
      double person,
      double address,
      double motion,
      boolean insideRegion) {
    return controller.observe(
        new PoseTriggerController.Observation(
            timestampMs * MS, person, address, motion, insideRegion));
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
