package com.agoessling.swingcapture.pose;

import java.util.ArrayList;
import java.util.List;

/** Production-lifecycle coverage for field pose replay. */
public final class PoseFieldSessionReplayTest {
  private static final long MS = 1_000_000L;

  private PoseFieldSessionReplayTest() {}

  public static void main(String[] arguments) {
    poseIsBlindWhileHighSpeedRunsAndCapturesAReadyTarget();
    practiceSwingBeforeRealImpactUsesOneArmedAttempt();
    postShotFinishWaitsForResetThenRepeatedSetupCaptures();
    lateStartupIsReportedAsAMiss();
    armWithoutSwingTimesOutThenRepeatedSetupCaptures();
  }

  private static void poseIsBlindWhileHighSpeedRunsAndCapturesAReadyTarget() {
    PoseFieldSessionReplay.Result result =
        PoseFieldSessionReplay.evaluate(
            observations(0, 4_000, 1_000, 0),
            List.of(new PoseFieldSessionReplay.Target("S01", 2_000 * MS, 1_800 * MS)),
            config(300, 500, 200));

    check(result.capturedTargetCount() == 1, "ready target captured");
    check(result.attempts().size() == 1, "one attempt");
    check(result.attempts().get(0).armNs() == 1_400 * MS, "arm timestamp");
    check(result.attempts().get(0).completionNs() == 2_500 * MS, "capture completion");
    check(result.skippedPoseObservations() > 0, "high-speed observations skipped");
  }

  private static void practiceSwingBeforeRealImpactUsesOneArmedAttempt() {
    ArrayList<PoseTriggerController.Observation> observations = new ArrayList<>();
    for (int timestampMs = 0; timestampMs <= 10_000; timestampMs += 200) {
      boolean initialSetup = timestampMs >= 400 && timestampMs <= 1_000;
      boolean practiceSwing = timestampMs >= 1_200 && timestampMs <= 3_000;
      boolean realSetup = timestampMs >= 6_000;
      double address = initialSetup || realSetup ? 0.9 : 0.1;
      double motion = practiceSwing ? 0.9 : 0.1;
      observations.add(observation(timestampMs, address, motion));
    }
    PoseFieldSessionReplay.Result result =
        PoseFieldSessionReplay.evaluate(
            observations,
            List.of(new PoseFieldSessionReplay.Target("S01", 8_000 * MS, 7_800 * MS)),
            config(200, 400, 0));

    check(result.capturedTargetCount() == 1, "real impact after practice swing captured");
    check(result.attempts().size() == 1, "practice swing does not create a second attempt");
    check(result.attempts().get(0).armNs() == 800 * MS, "initial setup owns the attempt");
    check(
        result.attempts().get(0).outcome()
            == PoseFieldSessionReplay.AttemptOutcome.IMPACT_CAPTURED,
        "armed attempt survives until real impact");
  }

  private static void postShotFinishWaitsForResetThenRepeatedSetupCaptures() {
    ArrayList<PoseTriggerController.Observation> observations = new ArrayList<>();
    for (int timestampMs = 0; timestampMs <= 6_000; timestampMs += 200) {
      boolean firstAddress = timestampMs >= 600 && timestampMs <= 1_600;
      boolean postShotFinish = timestampMs >= 2_200 && timestampMs <= 3_800;
      boolean secondAddress = timestampMs >= 4_200;
      double address = firstAddress || postShotFinish || secondAddress ? 0.9 : 0.1;
      double motion = firstAddress || postShotFinish || secondAddress ? 0.1 : 0.9;
      observations.add(observation(timestampMs, address, motion));
    }
    PoseFieldSessionReplay.Result result =
        PoseFieldSessionReplay.evaluate(
            observations,
            List.of(
                new PoseFieldSessionReplay.Target("S01", 1_600 * MS, 1_400 * MS),
                new PoseFieldSessionReplay.Target("S02", 5_400 * MS, 5_200 * MS)),
            config(200, 400, 0));

    check(result.capturedTargetCount() == 2, "two in-frame swings captured");
    check(result.attempts().size() == 2, "two distinct attempts");
    check(result.attempts().get(1).armNs() == 4_600 * MS, "finish did not create a false attempt");
  }

  private static void lateStartupIsReportedAsAMiss() {
    PoseFieldSessionReplay.Result result =
        PoseFieldSessionReplay.evaluate(
            observations(0, 3_000, 1_000, 0),
            List.of(new PoseFieldSessionReplay.Target("S01", 2_000 * MS, 1_700 * MS)),
            config(400, 500, 0));

    check(result.capturedTargetCount() == 0, "late target missed");
    check(
        result.targets().get(0).outcome()
            == PoseFieldSessionReplay.ImpactOutcome.VIDEO_STARTED_AFTER_TAKEAWAY,
        "late-start outcome");
  }

  private static void armWithoutSwingTimesOutThenRepeatedSetupCaptures() {
    PoseTriggerController.Config defaults =
        PoseTriggerController.Config.defaultsForFiveFramesPerSecond();
    PoseTriggerController.Config shortAttempt =
        new PoseTriggerController.Config(
            defaults.minimumPersonConfidence(),
            defaults.maximumClearPersonConfidence(),
            defaults.minimumAddressConfidence(),
            defaults.maximumMotionMagnitude(),
            defaults.minimumQualificationNs(),
            defaults.maximumObservationGapNs(),
            defaults.qualificationDropoutGraceNs(),
            1_000 * MS,
            defaults.clearDurationNs(),
            0,
            2_000 * MS);
    PoseFieldSessionReplay.Config config =
        new PoseFieldSessionReplay.Config(
            shortAttempt, 100 * MS, 100 * MS, 100 * MS, 200 * MS, 100 * MS);

    ArrayList<PoseTriggerController.Observation> observations = new ArrayList<>();
    for (int timestampMs = 0; timestampMs <= 4_500; timestampMs += 200) {
      boolean noSwingSetup = timestampMs >= 400 && timestampMs <= 1_000;
      boolean repeatedSetup = timestampMs >= 2_600;
      double address = noSwingSetup || repeatedSetup ? 0.9 : 0.1;
      double motion = noSwingSetup || repeatedSetup ? 0.1 : 0.9;
      observations.add(observation(timestampMs, address, motion));
    }
    PoseFieldSessionReplay.Result result =
        PoseFieldSessionReplay.evaluate(
            observations,
            List.of(new PoseFieldSessionReplay.Target("S01", 3_600 * MS, 3_400 * MS)),
            config);

    check(result.attempts().size() == 2, "timeout and real swing are distinct attempts");
    check(
        result.attempts().get(0).outcome()
            == PoseFieldSessionReplay.AttemptOutcome.NO_IMPACT_TIMEOUT,
        "arm-without-swing timeout retained");
    check(result.attempts().get(0).armNs() == 800 * MS, "no-swing setup arm timestamp");
    check(
        result.attempts().get(1).outcome()
            == PoseFieldSessionReplay.AttemptOutcome.IMPACT_CAPTURED,
        "repeated setup captures after timeout and reset");
    check(result.attempts().get(1).armNs() == 3_000 * MS, "repeated setup arm timestamp");
    check(result.capturedTargetCount() == 1, "real target captured after false attempt");
  }

  private static List<PoseTriggerController.Observation> observations(
      int startMs, int endMs, int addressStartMs, int addressEndMs) {
    ArrayList<PoseTriggerController.Observation> observations = new ArrayList<>();
    for (int timestampMs = startMs; timestampMs <= endMs; timestampMs += 200) {
      boolean address =
          timestampMs >= addressStartMs
              && (addressEndMs == 0 || timestampMs <= addressEndMs);
      observations.add(observation(timestampMs, address ? 0.9 : 0.1, address ? 0.1 : 0.9));
    }
    return observations;
  }

  private static PoseTriggerController.Observation observation(
      int timestampMs, double address, double motion) {
    return new PoseTriggerController.Observation(timestampMs * MS, 0.95, address, motion, false);
  }

  private static PoseFieldSessionReplay.Config config(
      int startupMs, int completionMs, int restartMs) {
    return new PoseFieldSessionReplay.Config(
        PoseTriggerController.Config.defaultsForFiveFramesPerSecond(),
        startupMs * MS,
        startupMs * MS,
        completionMs * MS,
        completionMs * MS,
        restartMs * MS);
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
