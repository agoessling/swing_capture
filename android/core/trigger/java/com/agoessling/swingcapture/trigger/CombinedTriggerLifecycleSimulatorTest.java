package com.agoessling.swingcapture.trigger;

import com.agoessling.swingcapture.pose.PoseTriggerController;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/** Acceptance and boundary coverage for the leader-side pose/audio lifecycle replay. */
public final class CombinedTriggerLifecycleSimulatorTest {
  private static final long MS = 1_000_000L;

  private CombinedTriggerLifecycleSimulatorTest() {}

  public static void main(String[] arguments) throws Exception {
    fieldPatternCapturesTwelveRealSwingsAfterOnePracticeAttempt();
    offNominalTimelineRecoversWithoutLeavingFrame();
    poseLifecycleEdgeCasesRemainRecoverable();
    controllerDecisionOwnsDeadlinesWithoutReadinessMutation();
    localStartupTimeoutIsFatalAndItsExactBoundaryIsReady();
    jointReadinessUsesLocalAndPeerMilestonesAtTheImpact();
    unconfirmedPeerPreservesAudioButDefersNoImpactUntilFatalThermalCap();
    exactActiveDeadlineWinsOverAnAudioCandidate();
    walkingAndEmptySceneEvidenceNeverArms();
    targetsDuringCompletionAndRestartStayDistinct();
    impossibleStartupOrderingIsRejected();
    fixtureEndDoesNotShortenACompletionAlreadyInProgress();
    simulationIsDeterministic();
  }

  private static void fieldPatternCapturesTwelveRealSwingsAfterOnePracticeAttempt()
      throws Exception {
    CombinedTriggerFixture.Fixture fixture = load("field_twelve_with_practice.events");
    CombinedTriggerLifecycleSimulator.Result result =
        CombinedTriggerLifecycleSimulator.evaluate(
            fixture.scenario(), CombinedTriggerLifecycleSimulator.Config.pairedPixel6Reference());

    check(
        fixture.name().equals("field_twelve_with_initial_practice_swing"), "fixture name");
    check(result.failure().isEmpty(), "reference field replay has no fatal outcome");
    check(result.targets().size() == 12, "twelve labeled real swings");
    check(result.capturedTargetCount() == 12, "all twelve real swings captured");
    check(result.attempts().size() == 13, "practice plus twelve real attempts");
    check(result.falseAudioTerminalCount() == 1, "one separate practice terminal");
    check(
        result.attempts().get(0).terminalCandidateId().equals("PRACTICE"),
        "practice swing is the initial terminal");
    for (int index = 0; index < result.targets().size(); ++index) {
      CombinedTriggerLifecycleSimulator.TargetResult target = result.targets().get(index);
      check(target.id().equals("S%02d".formatted(index + 1)), "stable target id ordering");
      check(target.captured(), target.id() + " captured");
      check(target.videoLeadBeforeTakeawayNs() >= 3_000 * MS, target.id() + " video lead");
      check(
          target.triggerReadyLeadBeforeImpactNs() >= 900 * MS,
          target.id() + " trigger-readiness lead");
    }
    check(result.skippedPoseObservations() > 0, "high-speed observations are camera-blind");
    check(result.processedPoseObservations() > 0, "standby observations are processed");
  }

  private static void offNominalTimelineRecoversWithoutLeavingFrame() throws Exception {
    CombinedTriggerLifecycleSimulator.Result result =
        CombinedTriggerLifecycleSimulator.evaluate(
            load("off_nominal_lifecycles.events").scenario(), fastConfig());

    check(result.failure().isEmpty(), "recoverable timeline remains nonfatal");
    check(result.attempts().size() == 8, "all eight attempts retained");
    check(
        result.attempts().get(0).outcome()
            == CombinedTriggerLifecycleSimulator.AttemptOutcome.NO_IMPACT_TIMEOUT,
        "arm without impact reaches the controller deadline");
    check(result.attempts().get(0).activeEvidenceStopNs() == 4_400 * MS, "decision deadline");
    check(result.attempts().get(0).terminalNs() == 4_400 * MS, "no-impact terminal timing");
    check(result.attempts().get(1).armNs() == 7_400 * MS, "cooldown and reset rearm timing");
    check(result.attempts().get(1).videoStartedNs() == 7_600 * MS, "video startup timing");
    check(result.attempts().get(1).triggerReadyNs() == 8_000 * MS, "joint readiness timing");
    check(
        result.attempts().get(2).terminalCandidateId().equals("FALSE_BEFORE_T02"),
        "false audio is an explicit terminal rather than hidden inside T02");
    check(
        result.attempts().get(6).outcome()
            == CombinedTriggerLifecycleSimulator.AttemptOutcome.NO_IMPACT_TIMEOUT,
        "terminal peer failure releases the already-expired evidence timeout");
    check(result.attempts().get(6).activeEvidenceStopNs() == 33_400 * MS, "lease not extended");
    check(result.attempts().get(6).triggerReadyNs() == 35_400 * MS, "late peer resolution");
    check(result.attempts().get(6).terminalNs() == 35_400 * MS, "immediate late timeout");

    check(target(result, "T01").captured(), "rearm after no-impact timeout");
    check(
        target(result, "T02").outcome()
            == CombinedTriggerLifecycleSimulator.TargetOutcome.STANDBY_NOT_ARMED,
        "real impact after false terminal is classified as missed");
    check(target(result, "T03").captured(), "rearm after false terminal");
    check(target(result, "T04").captured(), "motion reset rearms without scene clear");
    check(target(result, "T05").captured(), "rapid back-to-back shot captures after cooldown");
    check(
        target(result, "T06").outcome()
            == CombinedTriggerLifecycleSimulator.TargetOutcome.AUDIO_TRIGGER_NOT_READY,
        "candidate before peer resolution is never accepted");
    check(target(result, "T07").captured(), "peer-failure timeout still permits later rearm");
    check(
        audio(result, "T06").disposition()
            == CombinedTriggerLifecycleSimulator.AudioDisposition.IGNORED_NOT_READY,
        "missed-readiness candidate disposition");
  }

  private static void poseLifecycleEdgeCasesRemainRecoverable() throws Exception {
    CombinedTriggerFixture.Fixture fixture = load("pose_lifecycle_edge_cases.events");
    CombinedTriggerLifecycleSimulator.Result result =
        CombinedTriggerLifecycleSimulator.evaluate(fixture.scenario(), fastConfig());

    check(fixture.name().equals("pose_lifecycle_edge_cases"), "lifecycle fixture name");
    check(result.failure().isEmpty(), "pose lifecycle remains recoverable");
    check(result.attempts().size() == 4, "timeout plus three real attempts");
    check(
        result.attempts().get(0).outcome()
            == CombinedTriggerLifecycleSimulator.AttemptOutcome.NO_IMPACT_TIMEOUT,
        "arm-without-swing terminates at its evidence deadline");
    check(result.attempts().get(0).armNs() == 3_600 * MS, "non-address cases never armed");
    check(result.attempts().get(0).terminalNs() == 6_600 * MS, "timeout deadline retained");

    check(result.attempts().get(1).armNs() == 9_400 * MS, "cooldown delays repeated setup");
    check(result.attempts().get(2).armNs() == 13_600 * MS, "in-frame reset permits rearm");
    check(result.attempts().get(3).armNs() == 17_400 * MS, "scene clear permits later rearm");
    check(result.capturedTargetCount() == 3, "all three real impacts captured");
    check(target(result, "S01").captured(), "S01 after no-impact recovery captured");
    check(target(result, "S02").captured(), "S02 after in-frame reset captured");
    check(target(result, "S03").captured(), "S03 after practice motion captured");
    check(
        result.skippedPoseObservations() >= 20,
        "pose observations during high-speed attempts are explicitly camera-blind");
    check(
        result.processedPoseObservations() >= 20,
        "empty, walk-through, abort, reset, and repeated setup observations are processed");
  }

  private static void controllerDecisionOwnsDeadlinesWithoutReadinessMutation() {
    CombinedTriggerLifecycleSimulator.StartupPlan lateReady =
        new CombinedTriggerLifecycleSimulator.StartupPlan(
            200 * MS,
            4_500 * MS,
            4_500 * MS,
            CombinedTriggerLifecycleSimulator.PeerArmResolution.FAILED);
    CombinedTriggerLifecycleSimulator.Result result =
        evaluate(
            events(pose(0), pose(200), pose(400)),
            Map.of(),
            5_500,
            config(poseConfig(3_000, 6_000), lateReady, true, 200, 200, 200));

    CombinedTriggerLifecycleSimulator.Attempt attempt = result.attempts().get(0);
    check(attempt.armNs() == 400 * MS, "arm timestamp comes from the production decision");
    check(attempt.activeEvidenceStopNs() == 3_400 * MS, "production evidence lease retained");
    check(attempt.thermalHardStopNs() == 6_400 * MS, "production thermal cap retained");
    check(attempt.triggerReadyNs() == 4_900 * MS, "late joint readiness represented separately");
    check(attempt.terminalNs() == 4_900 * MS, "expired lease executes when timeout becomes allowed");
    check(
        attempt.outcome() == CombinedTriggerLifecycleSimulator.AttemptOutcome.NO_IMPACT_TIMEOUT,
        "late terminal peer failure enables a recoverable no-impact timeout");
  }

  private static void localStartupTimeoutIsFatalAndItsExactBoundaryIsReady() {
    CombinedTriggerLifecycleSimulator.StartupPlan exact =
        new CombinedTriggerLifecycleSimulator.StartupPlan(200 * MS, 6_000 * MS);
    ArrayList<CombinedTriggerLifecycleSimulator.Event> exactEvents = armEvents();
    exactEvents.add(audio(6_400, "EXACT"));
    CombinedTriggerLifecycleSimulator.Result exactResult =
        evaluate(
            exactEvents,
            Map.of(),
            6_500,
            config(poseConfig(10_000, 20_000), exact, true, 100, 100, 100));
    check(exactResult.failure().isEmpty(), "local readiness at exactly six seconds succeeds");
    check(
        audio(exactResult, "EXACT").disposition()
            == CombinedTriggerLifecycleSimulator.AudioDisposition.TERMINATED_CAPTURE,
        "audio at the exact local-ready boundary is accepted");

    CombinedTriggerLifecycleSimulator.StartupPlan late =
        new CombinedTriggerLifecycleSimulator.StartupPlan(200 * MS, 6_000 * MS + 1);
    CombinedTriggerLifecycleSimulator.Result lateResult =
        evaluate(
            armEvents(),
            Map.of(),
            7_000,
            config(poseConfig(10_000, 20_000), late, true, 100, 100, 100));
    check(
        lateResult.failure().orElseThrow().reason()
            == CombinedTriggerLifecycleSimulator.FailureReason.LOCAL_STARTUP_TIMEOUT,
        "readiness one nanosecond late is a fatal startup failure");
    check(lateResult.failure().orElseThrow().timestampNs() == 6_400 * MS, "six-second cap");
    check(
        lateResult.attempts().get(0).outcome()
            == CombinedTriggerLifecycleSimulator.AttemptOutcome.LOCAL_STARTUP_FAILED,
        "failed startup remains explicit in attempt evidence");
  }

  private static void jointReadinessUsesLocalAndPeerMilestonesAtTheImpact() {
    CombinedTriggerLifecycleSimulator.StartupPlan plan =
        new CombinedTriggerLifecycleSimulator.StartupPlan(
            200 * MS,
            1_000 * MS,
            2_000 * MS,
            CombinedTriggerLifecycleSimulator.PeerArmResolution.READY);
    ArrayList<CombinedTriggerLifecycleSimulator.Event> events = armEvents();
    events.add(target("EARLY", 1_800, 2_000));
    events.add(audio(2_500, "EARLY"));
    CombinedTriggerLifecycleSimulator.Result early =
        evaluate(
            events,
            Map.of(),
            3_000,
            config(poseConfig(10_000, 20_000), plan, true, 100, 100, 100));
    check(early.attempts().get(0).localReadyNs() == 1_400 * MS, "local milestone");
    check(early.attempts().get(0).peerResolvedNs() == 2_400 * MS, "peer milestone");
    check(early.attempts().get(0).triggerReadyNs() == 2_400 * MS, "joint barrier");
    check(
        target(early, "EARLY").outcome()
            == CombinedTriggerLifecycleSimulator.TargetOutcome.AUDIO_TRIGGER_NOT_READY,
        "a later same-id terminal cannot credit an impact before joint readiness");

    ArrayList<CombinedTriggerLifecycleSimulator.Event> exactEvents = armEvents();
    exactEvents.add(audio(2_399, "BEFORE"));
    exactEvents.add(target("EXACT", 600, 2_400));
    exactEvents.add(audio(2_400, "EXACT"));
    CombinedTriggerLifecycleSimulator.Result exactResult =
        evaluate(
            exactEvents,
            Map.of(),
            3_000,
            config(poseConfig(10_000, 20_000), plan, true, 100, 100, 100));
    check(
        audio(exactResult, "BEFORE").disposition()
            == CombinedTriggerLifecycleSimulator.AudioDisposition.IGNORED_NOT_READY,
        "candidate one millisecond before the joint barrier is rejected");
    check(target(exactResult, "EXACT").captured(), "impact at joint readiness is captured");
  }

  private static void unconfirmedPeerPreservesAudioButDefersNoImpactUntilFatalThermalCap() {
    CombinedTriggerLifecycleSimulator.StartupPlan unconfirmed =
        new CombinedTriggerLifecycleSimulator.StartupPlan(
            200 * MS,
            600 * MS,
            1_000 * MS,
            CombinedTriggerLifecycleSimulator.PeerArmResolution.UNCONFIRMED);
    ArrayList<CombinedTriggerLifecycleSimulator.Event> acceptedEvents = armEvents();
    acceptedEvents.add(audio(1_400, "DEGRADED"));
    CombinedTriggerLifecycleSimulator.Result accepted =
        evaluate(
            acceptedEvents,
            Map.of(),
            2_000,
            config(poseConfig(3_000, 6_000), unconfirmed, true, 100, 100, 100));
    check(
        audio(accepted, "DEGRADED").disposition()
            == CombinedTriggerLifecycleSimulator.AudioDisposition.TERMINATED_CAPTURE,
        "UNCONFIRMED resolution still opens degraded local audio capture");

    ArrayList<CombinedTriggerLifecycleSimulator.Event> fatalEvents = armEvents();
    fatalEvents.add(target("AFTER_FATAL", 6_500, 6_700));
    fatalEvents.add(audio(6_700, "AFTER_FATAL"));
    CombinedTriggerLifecycleSimulator.Result result =
        evaluate(
            fatalEvents,
            Map.of(),
            7_000,
            config(poseConfig(3_000, 6_000), unconfirmed, true, 100, 100, 100));
    check(result.attempts().get(0).triggerReadyNs() == 1_400 * MS, "degraded audio gate opens");
    check(
        result.failure().orElseThrow().reason()
            == CombinedTriggerLifecycleSimulator.FailureReason.THERMAL_HARD_CAP,
        "unconfirmed peer suppresses recoverable no-impact publication until thermal failure");
    check(result.failure().orElseThrow().timestampNs() == 6_400 * MS, "absolute hard cap");
    check(
        result.attempts().get(0).outcome()
            == CombinedTriggerLifecycleSimulator.AttemptOutcome.THERMAL_HARD_CAP,
        "thermal hard cap is fatal, not a completion/restart path");
    check(result.attempts().size() == 1, "fatal thermal cap cannot recover or rearm");
    check(
        target(result, "AFTER_FATAL").outcome()
            == CombinedTriggerLifecycleSimulator.TargetOutcome.SESSION_FAILED,
        "targets after the hard cap retain the fatal session state");
    check(
        audio(result, "AFTER_FATAL").disposition()
            == CombinedTriggerLifecycleSimulator.AudioDisposition.IGNORED_FAILED,
        "audio after the hard cap cannot restart capture");
  }

  private static void exactActiveDeadlineWinsOverAnAudioCandidate() {
    CombinedTriggerLifecycleSimulator.StartupPlan ready =
        new CombinedTriggerLifecycleSimulator.StartupPlan(100 * MS, 200 * MS);
    ArrayList<CombinedTriggerLifecycleSimulator.Event> events = armEvents();
    events.add(audio(3_400, "BOUNDARY"));
    CombinedTriggerLifecycleSimulator.Result result =
        evaluate(
            events,
            Map.of(),
            4_000,
            config(poseConfig(3_000, 6_000), ready, true, 100, 100, 100));
    check(
        result.attempts().get(0).outcome()
            == CombinedTriggerLifecycleSimulator.AttemptOutcome.NO_IMPACT_TIMEOUT,
        "active deadline is inclusive");
    check(
        audio(result, "BOUNDARY").disposition()
            == CombinedTriggerLifecycleSimulator.AudioDisposition.IGNORED_COMPLETING,
        "candidate at the deadline cannot race the timeout in deterministic replay");
  }

  private static void walkingAndEmptySceneEvidenceNeverArms() {
    ArrayList<CombinedTriggerLifecycleSimulator.Event> events =
        events(
            observation(0, 0.05, 0.05, 0.05),
            observation(200, 0.95, 0.10, 0.80),
            observation(400, 0.95, 0.20, 0.70),
            observation(600, 0.05, 0.05, 0.05));
    events.add(target("WALK", 800, 1_000));
    events.add(audio(1_000, "WALK"));
    CombinedTriggerLifecycleSimulator.Result result =
        evaluate(
            events,
            Map.of(),
            1_200,
            config(
                poseConfig(3_000, 6_000),
                new CombinedTriggerLifecycleSimulator.StartupPlan(100 * MS, 200 * MS),
                true,
                100,
                100,
                100));
    check(result.attempts().isEmpty(), "empty/walk-through observations never arm high speed");
    check(
        target(result, "WALK").outcome()
            == CombinedTriggerLifecycleSimulator.TargetOutcome.STANDBY_NOT_ARMED,
        "walk-through target is not credited");
    check(
        audio(result, "WALK").disposition()
            == CombinedTriggerLifecycleSimulator.AudioDisposition.IGNORED_STANDBY,
        "walk-through audio remains outside an attempt");
  }

  private static void targetsDuringCompletionAndRestartStayDistinct() {
    ArrayList<CombinedTriggerLifecycleSimulator.Event> events = armEvents();
    events.add(audio(1_000, "FALSE"));
    events.add(target("COMPLETING", 1_100, 1_200));
    events.add(target("RESTARTING", 1_600, 1_700));
    CombinedTriggerLifecycleSimulator.Result result =
        evaluate(
            events,
            Map.of(),
            2_200,
            config(
                poseConfig(3_000, 6_000),
                new CombinedTriggerLifecycleSimulator.StartupPlan(100 * MS, 200 * MS),
                true,
                500,
                100,
                500));
    check(
        target(result, "COMPLETING").outcome()
            == CombinedTriggerLifecycleSimulator.TargetOutcome.PREVIOUS_ATTEMPT_COMPLETING,
        "post-terminal completion is distinct from standby");
    check(
        target(result, "RESTARTING").outcome()
            == CombinedTriggerLifecycleSimulator.TargetOutcome.CAMERA_RESTARTING,
        "camera restart is distinct from completion and standby");
  }

  private static void impossibleStartupOrderingIsRejected() {
    expectIllegalArgument(
        () -> new CombinedTriggerLifecycleSimulator.StartupPlan(500 * MS, 499 * MS),
        "local readiness cannot precede video");
    expectIllegalArgument(
        () ->
            new CombinedTriggerLifecycleSimulator.StartupPlan(
                100 * MS,
                200 * MS,
                -1,
                CombinedTriggerLifecycleSimulator.PeerArmResolution.READY),
        "missing peer milestone must remain pending");
  }

  private static void fixtureEndDoesNotShortenACompletionAlreadyInProgress() {
    CombinedTriggerLifecycleSimulator.StartupPlan ready =
        new CombinedTriggerLifecycleSimulator.StartupPlan(100 * MS, 200 * MS);
    ArrayList<CombinedTriggerLifecycleSimulator.Event> events = armEvents();
    events.add(audio(1_000, "POSTROLL"));
    CombinedTriggerLifecycleSimulator.Result result =
        evaluate(
            events,
            Map.of(),
            1_500,
            config(poseConfig(3_000, 6_000), ready, true, 2_000, 100, 100));
    CombinedTriggerLifecycleSimulator.Attempt attempt = result.attempts().get(0);
    check(
        attempt.outcome() == CombinedTriggerLifecycleSimulator.AttemptOutcome.AUDIO_TERMINAL,
        "terminal identity survives fixture end");
    check(attempt.completionNs() == 3_000 * MS, "known post-roll completion is not truncated");
  }

  private static void simulationIsDeterministic() throws Exception {
    CombinedTriggerFixture.Fixture fixture = load("off_nominal_lifecycles.events");
    CombinedTriggerLifecycleSimulator.Result first =
        CombinedTriggerLifecycleSimulator.evaluate(fixture.scenario(), fastConfig());
    CombinedTriggerLifecycleSimulator.Result second =
        CombinedTriggerLifecycleSimulator.evaluate(fixture.scenario(), fastConfig());
    check(first.equals(second), "the same fixture and config produce the same complete result");
  }

  private static CombinedTriggerLifecycleSimulator.Config fastConfig() {
    return config(
        poseConfig(3_000, 6_000),
        new CombinedTriggerLifecycleSimulator.StartupPlan(200 * MS, 600 * MS),
        true,
        200,
        200,
        200);
  }

  private static CombinedTriggerLifecycleSimulator.Config config(
      PoseTriggerController.Config poseConfig,
      CombinedTriggerLifecycleSimulator.StartupPlan startup,
      boolean paired,
      long postAudioMs,
      long timeoutMs,
      long restartMs) {
    return new CombinedTriggerLifecycleSimulator.Config(
        poseConfig,
        startup,
        paired,
        6_000 * MS,
        postAudioMs * MS,
        timeoutMs * MS,
        restartMs * MS);
  }

  private static PoseTriggerController.Config poseConfig(long activeMs, long thermalMs) {
    PoseTriggerController.Config defaults =
        PoseTriggerController.Config.defaultsForFiveFramesPerSecond();
    return new PoseTriggerController.Config(
        defaults.minimumPersonConfidence(),
        defaults.maximumClearPersonConfidence(),
        defaults.minimumAddressConfidence(),
        defaults.maximumMotionMagnitude(),
        defaults.minimumQualificationNs(),
        defaults.maximumObservationGapNs(),
        defaults.qualificationDropoutGraceNs(),
        activeMs * MS,
        defaults.clearDurationNs(),
        defaults.cooldownNs(),
        thermalMs * MS);
  }

  private static CombinedTriggerLifecycleSimulator.Result evaluate(
      List<CombinedTriggerLifecycleSimulator.Event> events,
      Map<Integer, CombinedTriggerLifecycleSimulator.StartupPlan> overrides,
      long endMs,
      CombinedTriggerLifecycleSimulator.Config config) {
    return CombinedTriggerLifecycleSimulator.evaluate(
        new CombinedTriggerLifecycleSimulator.Scenario(events, overrides, endMs * MS), config);
  }

  private static ArrayList<CombinedTriggerLifecycleSimulator.Event> armEvents() {
    return events(pose(0), pose(200), pose(400));
  }

  private static ArrayList<CombinedTriggerLifecycleSimulator.Event> events(
      CombinedTriggerLifecycleSimulator.Event... events) {
    return new ArrayList<>(List.of(events));
  }

  private static CombinedTriggerLifecycleSimulator.PoseEvent pose(long timestampMs) {
    return observation(timestampMs, 0.95, 0.90, 0.10);
  }

  private static CombinedTriggerLifecycleSimulator.PoseEvent observation(
      long timestampMs, double person, double address, double motion) {
    return new CombinedTriggerLifecycleSimulator.PoseEvent(
        timestampMs * MS, person, address, motion);
  }

  private static CombinedTriggerLifecycleSimulator.AudioCandidateEvent audio(
      long timestampMs, String id) {
    return new CombinedTriggerLifecycleSimulator.AudioCandidateEvent(timestampMs * MS, id);
  }

  private static CombinedTriggerLifecycleSimulator.TargetEvent target(
      String id, long takeawayMs, long impactMs) {
    return new CombinedTriggerLifecycleSimulator.TargetEvent(id, takeawayMs * MS, impactMs * MS);
  }

  private static CombinedTriggerLifecycleSimulator.TargetResult target(
      CombinedTriggerLifecycleSimulator.Result result, String id) {
    return result.targets().stream()
        .filter(target -> target.id().equals(id))
        .findFirst()
        .orElseThrow(() -> new AssertionError("missing target " + id));
  }

  private static CombinedTriggerLifecycleSimulator.AudioCandidateResult audio(
      CombinedTriggerLifecycleSimulator.Result result, String id) {
    return result.audioCandidates().stream()
        .filter(candidate -> candidate.candidateId().equals(id))
        .findFirst()
        .orElseThrow(() -> new AssertionError("missing audio candidate " + id));
  }

  private static CombinedTriggerFixture.Fixture load(String filename) throws Exception {
    String testSourceDirectory = System.getenv("TEST_SRCDIR");
    String workspace = System.getenv("TEST_WORKSPACE");
    Path path =
        testSourceDirectory == null || workspace == null
            ? Path.of("android/core/trigger/testdata", filename)
            : Path.of(testSourceDirectory, workspace, "android/core/trigger/testdata", filename);
    return CombinedTriggerFixture.load(path);
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }

  private static void expectIllegalArgument(Runnable operation, String message) {
    try {
      operation.run();
      throw new AssertionError(message);
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
  }
}
