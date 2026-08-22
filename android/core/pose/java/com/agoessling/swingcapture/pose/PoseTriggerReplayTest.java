package com.agoessling.swingcapture.pose;

import java.util.ArrayList;
import java.util.List;

/** Golden scoring coverage for offline pose-trigger replay. */
public final class PoseTriggerReplayTest {
  private static final long MS = 1_000_000L;

  private PoseTriggerReplayTest() {}

  public static void main(String[] arguments) {
    passesWhenHighSpeedIsReadyBeforeTakeaway();
    failsLateArmUsingStartupBudget();
    rejectsFinishPoseInForbiddenInterval();
    reportsMissingArm();
    reportsSignedOffsetFromPreferredArm();
    treatsEarlyButSafeArmAsAcceptable();
    scoresMultipleArmRequestsWithoutThrowing();
  }

  private static void passesWhenHighSpeedIsReadyBeforeTakeaway() {
    PoseTriggerReplay.Result result =
        PoseTriggerReplay.evaluate(
            config(),
            observationsWithAddressStartingAt(1_000),
            annotation(800, 2_000, 300, List.of(new PoseTriggerReplay.Interval(0, 800 * MS))));

    check(result.armRequestNs().orElseThrow() == 1_400 * MS, "pass arm time");
    check(result.highSpeedReadyNs().orElseThrow() == 1_700 * MS, "pass ready time");
    check(result.readyLeadBeforeTakeawayNs().orElseThrow() == 300 * MS, "pass lead");
    check(result.readyByTakeaway(), "pass readiness");
    check(result.passed(), "pass result");
    check(result.outcome() == PoseTriggerReplay.Outcome.ACCEPTABLE, "pass outcome");
  }

  private static void failsLateArmUsingStartupBudget() {
    PoseTriggerReplay.Result result =
        PoseTriggerReplay.evaluate(
            config(), observationsWithAddressStartingAt(1_400), annotation(800, 2_000, 300, List.of()));

    check(result.armRequestNs().orElseThrow() == 1_800 * MS, "late arm time");
    check(result.readyLeadBeforeTakeawayNs().orElseThrow() == -100 * MS, "late negative lead");
    check(!result.readyByTakeaway(), "late readiness");
    check(!result.passed(), "late result");
    check(
        result.outcome() == PoseTriggerReplay.Outcome.NOT_READY_BY_TAKEAWAY,
        "late outcome");
  }

  private static void rejectsFinishPoseInForbiddenInterval() {
    PoseTriggerReplay.Result result =
        PoseTriggerReplay.evaluate(
            config(),
            observationsWithAddressStartingAt(1_000),
            annotation(
                800,
                3_000,
                100,
                List.of(new PoseTriggerReplay.Interval(1_300 * MS, 2_000 * MS))));

    check(result.armedInForbiddenInterval(), "forbidden arm flag");
    check(!result.passed(), "forbidden result");
    check(result.outcome() == PoseTriggerReplay.Outcome.FORBIDDEN_ARM, "forbidden outcome");
  }

  private static void reportsMissingArm() {
    List<PoseTriggerController.Observation> observations = new ArrayList<>();
    for (int timestampMs = 0; timestampMs <= 2_000; timestampMs += 200) {
      observations.add(observation(timestampMs, 0.9, 0.2, 0.0, true));
    }

    PoseTriggerReplay.Result result =
        PoseTriggerReplay.evaluate(config(), observations, annotation(500, 2_000, 100, List.of()));

    check(result.armRequestNs().isEmpty(), "missing arm time");
    check(!result.passed(), "missing arm result");
    check(result.outcome() == PoseTriggerReplay.Outcome.NO_ARM_REQUEST, "missing outcome");
  }

  private static void reportsSignedOffsetFromPreferredArm() {
    PoseTriggerReplay.Result early =
        PoseTriggerReplay.evaluate(
            config(),
            observationsWithAddressStartingAt(1_000),
            new PoseTriggerReplay.Annotation(800 * MS, 1_600 * MS, 2_000 * MS, 0, List.of()));
    check(
        early.armOffsetFromPreferredNs().orElseThrow() == -200 * MS,
        "early preferred-arm offset");

    PoseTriggerReplay.Result late =
        PoseTriggerReplay.evaluate(
            config(),
            observationsWithAddressStartingAt(1_400),
            new PoseTriggerReplay.Annotation(800 * MS, 1_600 * MS, 2_200 * MS, 0, List.of()));
    check(
        late.armOffsetFromPreferredNs().orElseThrow() == 200 * MS,
        "late preferred-arm offset");
  }

  private static void treatsEarlyButSafeArmAsAcceptable() {
    PoseTriggerReplay.Result result =
        PoseTriggerReplay.evaluate(
            config(),
            observationsWithAddressStartingAt(1_000),
            new PoseTriggerReplay.Annotation(
                800 * MS, 1_600 * MS, 2_000 * MS, 100 * MS, List.of()));

    check(result.armRequestNs().orElseThrow() == 1_400 * MS, "early-safe arm time");
    check(result.armOffsetFromPreferredNs().orElseThrow() == -200 * MS, "early-safe offset");
    check(result.passed(), "early-safe result");
    check(
        result.outcome() == PoseTriggerReplay.Outcome.ACCEPTABLE_EARLY,
        "early-safe outcome");
  }

  private static void scoresMultipleArmRequestsWithoutThrowing() {
    List<PoseTriggerController.Observation> observations = new ArrayList<>();
    for (int timestampMs = 0; timestampMs <= 5_200; timestampMs += 200) {
      boolean firstSetup = timestampMs >= 1_000 && timestampMs <= 1_400;
      boolean clear = timestampMs >= 1_600 && timestampMs <= 4_600;
      boolean secondSetup = timestampMs >= 4_800;
      if (firstSetup || secondSetup) {
        observations.add(observation(timestampMs, 0.9, 0.9, 0.0, true));
      } else if (clear) {
        observations.add(observation(timestampMs, 0.0, 0.0, 0.0, false));
      } else {
        observations.add(observation(timestampMs, 0.9, 0.2, 0.0, true));
      }
    }

    PoseTriggerReplay.Result result =
        PoseTriggerReplay.evaluate(
            config(), observations, annotation(800, 7_000, 100, List.of()));

    check(result.armRequestCount() == 2, "multiple arm count");
    check(result.armRequestNs().orElseThrow() == 1_400 * MS, "multiple first arm time");
    check(result.passed(), "multiple replay result");
  }

  private static List<PoseTriggerController.Observation> observationsWithAddressStartingAt(
      int addressStartMs) {
    List<PoseTriggerController.Observation> observations = new ArrayList<>();
    for (int timestampMs = 0; timestampMs <= 2_200; timestampMs += 200) {
      double address = timestampMs >= addressStartMs ? 0.85 : 0.2;
      observations.add(observation(timestampMs, 0.9, address, 0.05, true));
    }
    return observations;
  }

  private static PoseTriggerController.Observation observation(
      int timestampMs,
      double person,
      double address,
      double motion,
      boolean insideRegion) {
    return new PoseTriggerController.Observation(
        timestampMs * MS, person, address, motion, insideRegion);
  }

  private static PoseTriggerReplay.Annotation annotation(
      int safeStartMs,
      int takeawayMs,
      int startupBudgetMs,
      List<PoseTriggerReplay.Interval> forbidden) {
    return new PoseTriggerReplay.Annotation(
        safeStartMs * MS, takeawayMs * MS, startupBudgetMs * MS, forbidden);
  }

  private static PoseTriggerController.Config config() {
    return PoseTriggerController.Config.defaultsForFiveFramesPerSecond();
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
