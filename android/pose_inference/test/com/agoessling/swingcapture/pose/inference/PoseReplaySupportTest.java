package com.agoessling.swingcapture.pose.inference;

import com.agoessling.swingcapture.pose.NormalizedHittingRegion;
import com.agoessling.swingcapture.pose.NormalizedPoseLandmark;
import com.agoessling.swingcapture.pose.PoseJoint;
import com.agoessling.swingcapture.pose.PoseLandmarkFrame;
import com.agoessling.swingcapture.pose.PoseProjection;
import com.agoessling.swingcapture.pose.PoseTriggerController;
import java.util.ArrayList;
import java.util.EnumMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.OptionalLong;

public final class PoseReplaySupportTest {
  private static final long MS = 1_000_000L;
  private static final NormalizedHittingRegion REGION =
      new NormalizedHittingRegion(0.25, 0.40, 0.75, 0.95);

  public static void main(String[] args) {
    runsExactExtractorAndControllerPath();
    appliesArmExpectationsWithoutHidingOutcome();
    serializesFailClosedRuntimeReport();
    rejectsContradictoryAndUnboundedReports();
  }

  private static void runsExactExtractorAndControllerPath() {
    PoseReplayConfiguration configuration = configuration(PoseReplayConfiguration.Expectation.REQUIRE_ARM);
    PoseReplayEvaluation evaluation = new PoseReplayEvaluation(configuration);
    for (int index = 0; index < 3; index++) {
      evaluation.accept(frame(index * 200L), 10L + index * 10L, 5L);
    }
    PoseInferenceMetrics.Snapshot metrics = new PoseInferenceMetrics.Snapshot(3, 0, 3, 0, 3, 0, 15, 5);
    PoseReplayReport report =
        PoseReplayReport.completed(
            "address.mp4",
            configuration,
            PoseInferenceDelegate.CPU,
            metrics,
            evaluation.trace(),
            evaluation.finalState());

    check(report.passed(), "required arm passes");
    check(report.outcome() == PoseReplayReport.Outcome.ARM_REQUESTED, "arm outcome");
    check(report.armRequestCount() == 1, "one arm request");
    check(report.firstArmRequestNs().orElseThrow() == 400 * MS, "arm timestamp");
    check(
        report.trace().get(2).decision().transitionReason()
            == PoseTriggerController.TransitionReason.ADDRESS_STABLE_ARMED,
        "exact controller transition");
    PoseReplayReportValidator.validate(report);
    String json = report.toJson();
    check(json.contains("\"report_type\":\"pose_replay_hil\""), "report type");
    check(json.contains("\"arm_request_count\":1"), "serialized arm count");
    check(json.contains("\"source_timestamp_ns\":400000000"), "serialized timestamp");
    check(json.length() < PoseReplayReportValidator.MAXIMUM_REPORT_CHARACTERS, "bounded JSON");
  }

  private static void appliesArmExpectationsWithoutHidingOutcome() {
    PoseReplayConfiguration configuration =
        configuration(PoseReplayConfiguration.Expectation.REQUIRE_NO_ARM);
    PoseReplayEvaluation evaluation = new PoseReplayEvaluation(configuration);
    for (int index = 0; index < 3; index++) {
      evaluation.accept(frame(index * 200L), index, 1);
    }
    PoseReplayReport report =
        PoseReplayReport.completed(
            "unexpected-arm.mp4",
            configuration,
            PoseInferenceDelegate.GPU,
            new PoseInferenceMetrics.Snapshot(3, 0, 3, 0, 3, 0, 3, 1),
            evaluation.trace(),
            evaluation.finalState());
    check(!report.passed(), "unexpected arm fails");
    check(report.outcome() == PoseReplayReport.Outcome.ARM_REQUESTED, "outcome remains auditable");
    check(
        report.failureCode() == PoseReplayReport.FailureCode.UNEXPECTED_ARM_REQUEST,
        "expectation failure code");
  }

  private static void serializesFailClosedRuntimeReport() {
    PoseReplayReport report =
        PoseReplayReport.failed(
            "broken.mp4",
            configuration(PoseReplayConfiguration.Expectation.OBSERVE_ONLY),
            Optional.empty(),
            new PoseInferenceMetrics.Snapshot(0, 0, 0, 0, 0, 0, 0, 0),
            List.of(),
            PoseTriggerController.State.WATCHING,
            PoseReplayReport.FailureCode.RUNTIME_FAILURE);
    check(!report.passed(), "runtime failure closed");
    check(report.toJson().contains("\"actual_delegate\":null"), "unavailable delegate");
    check(report.toJson().contains("\"first_source_timestamp_ns\":null"), "no false timestamp");
  }

  private static void rejectsContradictoryAndUnboundedReports() {
    expectThrows(
        IllegalArgumentException.class,
        () ->
            PoseReplayConfiguration.defaults(
                "face_on",
                PoseProjection.DOWN_THE_LINE,
                REGION,
                PoseInferenceDelegatePolicy.CPU_ONLY,
                PoseReplayConfiguration.Expectation.OBSERVE_ONLY));
    expectThrows(
        IllegalArgumentException.class,
        () ->
            new PoseReplayConfiguration(
                "face_on",
                PoseProjection.ACROSS_THE_LINE,
                REGION,
                PoseInferenceDelegatePolicy.CPU_ONLY,
                com.agoessling.swingcapture.pose.PoseLandmarkObservationExtractor.Config
                    .defaultsForFiveFramesPerSecond(),
                PoseTriggerController.Config.defaultsForFiveFramesPerSecond(),
                PoseReplayConfiguration.Expectation.OBSERVE_ONLY,
                PoseReplayReportValidator.MAXIMUM_TRACE_FRAMES + 1));

    PoseReplayConfiguration configuration = configuration(PoseReplayConfiguration.Expectation.REQUIRE_ARM);
    PoseReplayEvaluation evaluation = new PoseReplayEvaluation(configuration);
    for (int index = 0; index < 3; index++) {
      evaluation.accept(frame(index * 200L), index, 1);
    }
    List<PoseReplayReport.FrameTrace> invalidTrace = new ArrayList<>(evaluation.trace());
    PoseReplayReport.FrameTrace first = invalidTrace.get(0);
    invalidTrace.set(
        0,
        new PoseReplayReport.FrameTrace(
            1,
            first.sourceTimestampNs(),
            first.inferenceStartedNs(),
            first.inferenceDurationNs(),
            first.poseCount(),
            first.observation(),
            first.decision()));
    PoseReplayReport invalid =
        new PoseReplayReport(
            "invalid.mp4",
            configuration,
            Optional.of(PoseInferenceDelegate.CPU),
            new PoseInferenceMetrics.Snapshot(3, 0, 3, 0, 3, 0, 3, 1),
            invalidTrace,
            evaluation.finalState(),
            PoseReplayReport.Outcome.ARM_REQUESTED,
            PoseReplayReport.FailureCode.NONE);
    expectThrows(IllegalArgumentException.class, () -> PoseReplayReportValidator.validate(invalid));

    PoseTriggerController.Decision staleWatchingDecision =
        new PoseTriggerController.Decision(
            PoseTriggerController.State.WATCHING,
            PoseTriggerController.Command.NONE,
            "stale test decision",
            OptionalLong.empty(),
            OptionalLong.empty(),
            OptionalLong.of(10),
            OptionalLong.empty(),
            PoseTriggerController.TransitionReason.WAITING_FOR_ADDRESS);
    invalidTrace = new ArrayList<>(evaluation.trace());
    first = invalidTrace.get(0);
    invalidTrace.set(
        0,
        new PoseReplayReport.FrameTrace(
            first.sequenceIndex(),
            first.sourceTimestampNs(),
            first.inferenceStartedNs(),
            first.inferenceDurationNs(),
            first.poseCount(),
            first.observation(),
            staleWatchingDecision));
    PoseReplayReport staleState =
        new PoseReplayReport(
            "stale-state.mp4",
            configuration,
            Optional.of(PoseInferenceDelegate.CPU),
            new PoseInferenceMetrics.Snapshot(3, 0, 3, 0, 3, 0, 3, 1),
            invalidTrace,
            evaluation.finalState(),
            PoseReplayReport.Outcome.ARM_REQUESTED,
            PoseReplayReport.FailureCode.NONE);
    expectThrows(
        IllegalArgumentException.class, () -> PoseReplayReportValidator.validate(staleState));

    PoseReplayReport hiddenInferenceFailure =
        new PoseReplayReport(
            "hidden-failure.mp4",
            configuration,
            Optional.of(PoseInferenceDelegate.CPU),
            new PoseInferenceMetrics.Snapshot(4, 0, 4, 0, 3, 1, 4, 1),
            evaluation.trace(),
            evaluation.finalState(),
            PoseReplayReport.Outcome.ARM_REQUESTED,
            PoseReplayReport.FailureCode.NONE);
    expectThrows(
        IllegalArgumentException.class,
        () -> PoseReplayReportValidator.validate(hiddenInferenceFailure));
  }

  private static PoseReplayConfiguration configuration(
      PoseReplayConfiguration.Expectation expectation) {
    return PoseReplayConfiguration.defaults(
        "face_on",
        PoseProjection.ACROSS_THE_LINE,
        REGION,
        PoseInferenceDelegatePolicy.CPU_ONLY,
        expectation);
  }

  private static PoseLandmarkFrame frame(long timestampMs) {
    return new PoseLandmarkFrame(timestampMs * MS, 0.95, address());
  }

  private static Map<PoseJoint, NormalizedPoseLandmark> address() {
    EnumMap<PoseJoint, NormalizedPoseLandmark> pose = new EnumMap<>(PoseJoint.class);
    pose.put(PoseJoint.LEFT_SHOULDER, landmark(0.42, 0.30));
    pose.put(PoseJoint.RIGHT_SHOULDER, landmark(0.58, 0.30));
    pose.put(PoseJoint.LEFT_ELBOW, landmark(0.44, 0.44));
    pose.put(PoseJoint.RIGHT_ELBOW, landmark(0.56, 0.44));
    pose.put(PoseJoint.LEFT_WRIST, landmark(0.49, 0.59));
    pose.put(PoseJoint.RIGHT_WRIST, landmark(0.51, 0.59));
    pose.put(PoseJoint.LEFT_HIP, landmark(0.44, 0.51));
    pose.put(PoseJoint.RIGHT_HIP, landmark(0.56, 0.51));
    pose.put(PoseJoint.LEFT_KNEE, landmark(0.45, 0.68));
    pose.put(PoseJoint.RIGHT_KNEE, landmark(0.55, 0.68));
    pose.put(PoseJoint.LEFT_ANKLE, landmark(0.42, 0.86));
    pose.put(PoseJoint.RIGHT_ANKLE, landmark(0.58, 0.86));
    return pose;
  }

  private static NormalizedPoseLandmark landmark(double x, double y) {
    return new NormalizedPoseLandmark(x, y, 0.95);
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }

  private static void expectThrows(Class<? extends Throwable> type, Runnable action) {
    try {
      action.run();
    } catch (Throwable exception) {
      if (type.isInstance(exception)) {
        return;
      }
      throw new AssertionError("wrong exception", exception);
    }
    throw new AssertionError("expected " + type.getSimpleName());
  }
}
