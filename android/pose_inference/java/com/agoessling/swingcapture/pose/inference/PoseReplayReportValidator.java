package com.agoessling.swingcapture.pose.inference;

import com.agoessling.swingcapture.pose.PoseProjection;
import com.agoessling.swingcapture.pose.PoseTriggerController;
import java.util.List;
import java.util.Objects;

/** Public schema constants and semantic validation for Java and HIL report consumers. */
public final class PoseReplayReportValidator {
  public static final int SCHEMA_VERSION = 1;
  public static final String REPORT_TYPE = "pose_replay_hil";
  public static final int MAXIMUM_TRACE_FRAMES = 1_200;
  public static final int MAXIMUM_REPORT_CHARACTERS = 1_500_000;
  public static final int MAXIMUM_SOURCE_ID_CHARACTERS = 128;

  private PoseReplayReportValidator() {}

  public static void validateRoleProjection(String role, PoseProjection projection) {
    Objects.requireNonNull(role, "role");
    Objects.requireNonNull(projection, "projection");
    boolean valid =
        (role.equals("down_the_line") && projection == PoseProjection.DOWN_THE_LINE)
            || (role.equals("face_on") && projection == PoseProjection.ACROSS_THE_LINE);
    if (!valid) {
      throw new IllegalArgumentException("role and pose projection are inconsistent");
    }
  }

  public static void validate(PoseReplayReport report) {
    Objects.requireNonNull(report, "report");
    validateSourceId(report.sourceId());
    validateRoleProjection(report.configuration().role(), report.configuration().projection());
    List<PoseReplayReport.FrameTrace> trace = report.trace();
    if (trace.size() > report.configuration().maximumFrames()
        || trace.size() > MAXIMUM_TRACE_FRAMES) {
      throw new IllegalArgumentException("trace exceeds configured bounds");
    }

    long previousTimestampNs = -1;
    long successfulDurationNs = 0;
    long maximumSuccessfulDurationNs = 0;
    int armRequests = 0;
    for (int index = 0; index < trace.size(); index++) {
      PoseReplayReport.FrameTrace frame = Objects.requireNonNull(trace.get(index), "trace frame");
      if (frame.sequenceIndex() != index) {
        throw new IllegalArgumentException("trace sequence is not contiguous");
      }
      if (frame.sourceTimestampNs() < 0 || frame.sourceTimestampNs() <= previousTimestampNs) {
        throw new IllegalArgumentException("trace timestamps must increase strictly");
      }
      previousTimestampNs = frame.sourceTimestampNs();
      if (frame.inferenceStartedNs() < 0 || frame.inferenceDurationNs() < 0) {
        throw new IllegalArgumentException("trace inference timing cannot be negative");
      }
      if (frame.poseCount() < 0 || frame.poseCount() > 1) {
        throw new IllegalArgumentException("Lite replay trace supports zero or one pose");
      }
      if (frame.observation().timestampNs() != frame.sourceTimestampNs()) {
        throw new IllegalArgumentException("observation timestamp differs from source timestamp");
      }
      validateDecision(frame.decision(), frame.sourceTimestampNs());
      if (frame.decision().command() == PoseTriggerController.Command.START_HIGH_SPEED) {
        armRequests++;
      }
      successfulDurationNs = Math.addExact(successfulDurationNs, frame.inferenceDurationNs());
      maximumSuccessfulDurationNs =
          Math.max(maximumSuccessfulDurationNs, frame.inferenceDurationNs());
    }
    if (!trace.isEmpty()
        && report.finalState() != trace.get(trace.size() - 1).decision().state()) {
      throw new IllegalArgumentException("final state differs from the last decision");
    }

    PoseInferenceMetrics.Snapshot metrics = report.inferenceMetrics();
    requireNonnegative(metrics.offered(), "offered");
    requireNonnegative(metrics.cadenceRejected(), "cadenceRejected");
    requireNonnegative(metrics.scheduled(), "scheduled");
    requireNonnegative(metrics.droppedBackpressure(), "droppedBackpressure");
    requireNonnegative(metrics.succeeded(), "succeeded");
    requireNonnegative(metrics.failed(), "failed");
    requireNonnegative(metrics.totalInferenceNs(), "totalInferenceNs");
    requireNonnegative(metrics.maxInferenceNs(), "maxInferenceNs");
    if (metrics.cadenceRejected() != 0
        || metrics.droppedBackpressure() != 0
        || metrics.scheduled() != metrics.offered()
        || metrics.succeeded() != trace.size()
        || metrics.succeeded() + metrics.failed() != metrics.offered()) {
      throw new IllegalArgumentException("inference metrics contradict synchronous replay");
    }
    if (metrics.totalInferenceNs() < successfulDurationNs
        || metrics.maxInferenceNs() < maximumSuccessfulDurationNs) {
      throw new IllegalArgumentException("inference timing metrics contradict trace timing");
    }

    boolean runtimeFailure =
        report.failureCode() == PoseReplayReport.FailureCode.RUNTIME_FAILURE
            || report.failureCode() == PoseReplayReport.FailureCode.FRAME_LIMIT_EXCEEDED;
    if (runtimeFailure != (report.outcome() == PoseReplayReport.Outcome.RUNTIME_FAILURE)) {
      throw new IllegalArgumentException("runtime outcome and failure code disagree");
    }
    if (runtimeFailure) {
      return;
    }
    if (trace.isEmpty() || report.actualDelegate().isEmpty() || metrics.failed() != 0) {
      throw new IllegalArgumentException("completed replay lacks successful inference evidence");
    }
    PoseReplayReport.Outcome expectedOutcome =
        armRequests == 0
            ? PoseReplayReport.Outcome.NO_ARM_REQUEST
            : PoseReplayReport.Outcome.ARM_REQUESTED;
    if (report.outcome() != expectedOutcome) {
      throw new IllegalArgumentException("arm outcome contradicts controller trace");
    }
    PoseReplayReport.FailureCode expectedFailure =
        switch (report.configuration().expectation()) {
          case OBSERVE_ONLY -> PoseReplayReport.FailureCode.NONE;
          case REQUIRE_ARM ->
              armRequests == 0
                  ? PoseReplayReport.FailureCode.EXPECTED_ARM_NOT_OBSERVED
                  : PoseReplayReport.FailureCode.NONE;
          case REQUIRE_NO_ARM ->
              armRequests == 0
                  ? PoseReplayReport.FailureCode.NONE
                  : PoseReplayReport.FailureCode.UNEXPECTED_ARM_REQUEST;
        };
    if (report.failureCode() != expectedFailure) {
      throw new IllegalArgumentException("expectation result contradicts arm trace");
    }
  }

  private static void validateSourceId(String sourceId) {
    if (sourceId.isEmpty() || sourceId.length() > MAXIMUM_SOURCE_ID_CHARACTERS) {
      throw new IllegalArgumentException("sourceId is outside schema bounds");
    }
    for (int index = 0; index < sourceId.length(); index++) {
      char character = sourceId.charAt(index);
      if (character < 0x20 || character > 0x7e || character == '/' || character == '\\') {
        throw new IllegalArgumentException("sourceId must be a safe file name");
      }
    }
  }

  private static void validateDecision(
      PoseTriggerController.Decision decision, long sourceTimestampNs) {
    if (decision.transitionReason() == PoseTriggerController.TransitionReason.LEGACY) {
      throw new IllegalArgumentException("replay decisions require a machine-readable transition");
    }
    if (decision.qualificationStartedNs().isPresent()
        && decision.qualificationStartedNs().orElseThrow() > sourceTimestampNs) {
      throw new IllegalArgumentException("qualification starts after its decision");
    }
    if (decision.armRequestedNs().isPresent()
        && decision.armRequestedNs().orElseThrow() > sourceTimestampNs) {
      throw new IllegalArgumentException("arm request starts after its decision");
    }

    switch (decision.state()) {
      case WATCHING -> {
        requireEmpty(decision.qualificationStartedNs(), "watching qualification");
        requireEmpty(decision.armRequestedNs(), "watching arm request");
        requireEmpty(decision.activeUntilNs(), "watching active lease");
        requireEmpty(decision.thermalHardStopNs(), "watching thermal deadline");
      }
      case QUALIFYING -> {
        if (decision.qualificationStartedNs().isEmpty()) {
          throw new IllegalArgumentException("qualifying decision lacks its start timestamp");
        }
        requireEmpty(decision.armRequestedNs(), "qualifying arm request");
        requireEmpty(decision.activeUntilNs(), "qualifying active lease");
        requireEmpty(decision.thermalHardStopNs(), "qualifying thermal deadline");
      }
      case ARM_REQUESTED -> {
        requireEmpty(decision.qualificationStartedNs(), "armed qualification");
        if (decision.armRequestedNs().isEmpty()
            || decision.activeUntilNs().isEmpty()
            || decision.thermalHardStopNs().isEmpty()) {
          throw new IllegalArgumentException("armed decision lacks its bounded active window");
        }
        long activeUntilNs = decision.activeUntilNs().orElseThrow();
        long thermalHardStopNs = decision.thermalHardStopNs().orElseThrow();
        if (activeUntilNs < sourceTimestampNs || activeUntilNs > thermalHardStopNs) {
          throw new IllegalArgumentException("armed decision has an invalid active window");
        }
      }
      case WAITING_FOR_CLEAR -> {
        requireEmpty(decision.qualificationStartedNs(), "waiting qualification");
        if (decision.armRequestedNs().isEmpty()) {
          throw new IllegalArgumentException("waiting decision lost its originating arm request");
        }
        requireEmpty(decision.activeUntilNs(), "waiting active lease");
        requireEmpty(decision.thermalHardStopNs(), "waiting thermal deadline");
      }
    }

    switch (decision.command()) {
      case START_HIGH_SPEED -> {
        if (decision.state() != PoseTriggerController.State.ARM_REQUESTED
            || decision.transitionReason()
                != PoseTriggerController.TransitionReason.ADDRESS_STABLE_ARMED
            || decision.armRequestedNs().orElseThrow() != sourceTimestampNs) {
          throw new IllegalArgumentException("start command contradicts its controller decision");
        }
      }
      case STOP_HIGH_SPEED -> {
        boolean stopReason =
            decision.transitionReason()
                    == PoseTriggerController.TransitionReason.ACTIVE_CLEAR_STOPPED
                || decision.transitionReason()
                    == PoseTriggerController.TransitionReason.ACTIVE_EVIDENCE_EXPIRED
                || decision.transitionReason()
                    == PoseTriggerController.TransitionReason.THERMAL_HARD_CAP_REACHED;
        if (decision.state() != PoseTriggerController.State.WAITING_FOR_CLEAR || !stopReason) {
          throw new IllegalArgumentException("stop command contradicts its controller decision");
        }
      }
      case NONE -> {}
    }
  }

  private static void requireEmpty(java.util.OptionalLong value, String name) {
    if (value.isPresent()) {
      throw new IllegalArgumentException(name + " must be absent");
    }
  }

  private static void requireNonnegative(long value, String name) {
    if (value < 0) {
      throw new IllegalArgumentException(name + " cannot be negative");
    }
  }
}
