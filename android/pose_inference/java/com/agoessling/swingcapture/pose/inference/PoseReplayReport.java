package com.agoessling.swingcapture.pose.inference;

import com.agoessling.swingcapture.pose.PoseLandmarkObservationExtractor;
import com.agoessling.swingcapture.pose.PoseTriggerController;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Objects;
import java.util.Optional;
import java.util.OptionalLong;

/** Bounded schema-v1 result from the exact on-device pose replay pipeline. */
public record PoseReplayReport(
    String sourceId,
    PoseReplayConfiguration configuration,
    Optional<PoseInferenceDelegate> actualDelegate,
    PoseInferenceMetrics.Snapshot inferenceMetrics,
    List<FrameTrace> trace,
    PoseTriggerController.State finalState,
    Outcome outcome,
    FailureCode failureCode) {
  public enum Outcome {
    ARM_REQUESTED,
    NO_ARM_REQUEST,
    RUNTIME_FAILURE
  }

  public enum FailureCode {
    NONE,
    RUNTIME_FAILURE,
    FRAME_LIMIT_EXCEEDED,
    EXPECTED_ARM_NOT_OBSERVED,
    UNEXPECTED_ARM_REQUEST
  }

  public record FrameTrace(
      int sequenceIndex,
      long sourceTimestampNs,
      long inferenceStartedNs,
      long inferenceDurationNs,
      int poseCount,
      PoseLandmarkObservationExtractor.Evaluation observation,
      PoseTriggerController.Decision decision) {
    public FrameTrace {
      Objects.requireNonNull(observation, "observation");
      Objects.requireNonNull(decision, "decision");
    }
  }

  public record ArmDecision(
      long sourceTimestampNs,
      PoseTriggerController.Command command,
      PoseTriggerController.State state,
      PoseTriggerController.TransitionReason transitionReason) {}

  public PoseReplayReport {
    Objects.requireNonNull(sourceId, "sourceId");
    Objects.requireNonNull(configuration, "configuration");
    Objects.requireNonNull(actualDelegate, "actualDelegate");
    Objects.requireNonNull(inferenceMetrics, "inferenceMetrics");
    trace = List.copyOf(Objects.requireNonNull(trace, "trace"));
    Objects.requireNonNull(finalState, "finalState");
    Objects.requireNonNull(outcome, "outcome");
    Objects.requireNonNull(failureCode, "failureCode");
  }

  public static PoseReplayReport completed(
      String sourceId,
      PoseReplayConfiguration configuration,
      PoseInferenceDelegate actualDelegate,
      PoseInferenceMetrics.Snapshot inferenceMetrics,
      List<FrameTrace> trace,
      PoseTriggerController.State finalState) {
    int armRequests = countArmRequests(trace);
    Outcome outcome = armRequests == 0 ? Outcome.NO_ARM_REQUEST : Outcome.ARM_REQUESTED;
    FailureCode failureCode = expectationFailure(configuration.expectation(), armRequests);
    PoseReplayReport report =
        new PoseReplayReport(
            sourceId,
            configuration,
            Optional.of(Objects.requireNonNull(actualDelegate, "actualDelegate")),
            inferenceMetrics,
            trace,
            finalState,
            outcome,
            failureCode);
    PoseReplayReportValidator.validate(report);
    return report;
  }

  public static PoseReplayReport failed(
      String sourceId,
      PoseReplayConfiguration configuration,
      Optional<PoseInferenceDelegate> actualDelegate,
      PoseInferenceMetrics.Snapshot inferenceMetrics,
      List<FrameTrace> trace,
      PoseTriggerController.State finalState,
      FailureCode failureCode) {
    if (failureCode == FailureCode.NONE
        || failureCode == FailureCode.EXPECTED_ARM_NOT_OBSERVED
        || failureCode == FailureCode.UNEXPECTED_ARM_REQUEST) {
      throw new IllegalArgumentException("failed replay requires a runtime failure code");
    }
    PoseReplayReport report =
        new PoseReplayReport(
            sourceId,
            configuration,
            actualDelegate,
            inferenceMetrics,
            trace,
            finalState,
            Outcome.RUNTIME_FAILURE,
            failureCode);
    PoseReplayReportValidator.validate(report);
    return report;
  }

  public boolean passed() {
    return failureCode == FailureCode.NONE;
  }

  public int armRequestCount() {
    return countArmRequests(trace);
  }

  public OptionalLong firstArmRequestNs() {
    return trace.stream()
        .filter(frame -> frame.decision().command() == PoseTriggerController.Command.START_HIGH_SPEED)
        .mapToLong(FrameTrace::sourceTimestampNs)
        .findFirst();
  }

  public OptionalLong firstSourceTimestampNs() {
    return trace.isEmpty() ? OptionalLong.empty() : OptionalLong.of(trace.get(0).sourceTimestampNs());
  }

  public OptionalLong lastSourceTimestampNs() {
    return trace.isEmpty()
        ? OptionalLong.empty()
        : OptionalLong.of(trace.get(trace.size() - 1).sourceTimestampNs());
  }

  public List<ArmDecision> armDecisions() {
    ArrayList<ArmDecision> decisions = new ArrayList<>();
    for (FrameTrace frame : trace) {
      if (frame.decision().command() != PoseTriggerController.Command.NONE) {
        decisions.add(
            new ArmDecision(
                frame.sourceTimestampNs(),
                frame.decision().command(),
                frame.decision().state(),
                frame.decision().transitionReason()));
      }
    }
    return List.copyOf(decisions);
  }

  public String toJson() {
    PoseReplayReportValidator.validate(this);
    StringBuilder json = new StringBuilder();
    json.append("{\"schema_version\":")
        .append(PoseReplayReportValidator.SCHEMA_VERSION)
        .append(",\"report_type\":\"")
        .append(PoseReplayReportValidator.REPORT_TYPE)
        .append("\",\"passed\":")
        .append(passed())
        .append(",\"outcome\":\"")
        .append(wire(outcome))
        .append("\",\"failure_code\":\"")
        .append(wire(failureCode))
        .append("\",\"source_id\":\"")
        .append(escape(sourceId))
        .append("\",\"role\":\"")
        .append(configuration.role())
        .append("\",\"projection\":\"")
        .append(configuration.projection().wireName())
        .append("\",\"delegate_policy\":\"")
        .append(wire(configuration.delegatePolicy()))
        .append("\",\"actual_delegate\":");
    if (actualDelegate.isPresent()) {
      json.append('"').append(wire(actualDelegate.orElseThrow())).append('"');
    } else {
      json.append("null");
    }
    json.append(",\"expectation\":\"")
        .append(wire(configuration.expectation()))
        .append("\",\"maximum_frames\":")
        .append(configuration.maximumFrames())
        .append(",\"hitting_region\":{\"left\":")
        .append(configuration.hittingRegion().left())
        .append(",\"top\":")
        .append(configuration.hittingRegion().top())
        .append(",\"right\":")
        .append(configuration.hittingRegion().right())
        .append(",\"bottom\":")
        .append(configuration.hittingRegion().bottom())
        .append("},\"frame_count\":")
        .append(trace.size())
        .append(",\"first_source_timestamp_ns\":");
    appendOptionalLong(json, firstSourceTimestampNs());
    json.append(",\"last_source_timestamp_ns\":");
    appendOptionalLong(json, lastSourceTimestampNs());
    json.append(",\"arm_request_count\":")
        .append(armRequestCount())
        .append(",\"first_arm_request_ns\":");
    appendOptionalLong(json, firstArmRequestNs());
    json.append(",\"final_state\":\"")
        .append(wire(finalState))
        .append("\",\"inference_metrics\":")
        .append(inferenceMetrics.toJson())
        .append(",\"arm_decisions\":[");
    List<ArmDecision> armDecisions = armDecisions();
    for (int index = 0; index < armDecisions.size(); index++) {
      if (index > 0) {
        json.append(',');
      }
      ArmDecision decision = armDecisions.get(index);
      json.append("{\"source_timestamp_ns\":")
          .append(decision.sourceTimestampNs())
          .append(",\"command\":\"")
          .append(wire(decision.command()))
          .append("\",\"state\":\"")
          .append(wire(decision.state()))
          .append("\",\"transition_reason\":\"")
          .append(wire(decision.transitionReason()))
          .append("\"}");
    }
    json.append("],\"trace\":[");
    for (int index = 0; index < trace.size(); index++) {
      if (index > 0) {
        json.append(',');
      }
      appendTrace(json, trace.get(index));
    }
    json.append("]}\n");
    if (json.length() > PoseReplayReportValidator.MAXIMUM_REPORT_CHARACTERS) {
      throw new IllegalStateException("pose replay report exceeded its schema bound");
    }
    return json.toString();
  }

  private static void appendTrace(StringBuilder json, FrameTrace frame) {
    PoseLandmarkObservationExtractor.Evaluation observation = frame.observation();
    PoseTriggerController.Decision decision = frame.decision();
    json.append("{\"sequence_index\":")
        .append(frame.sequenceIndex())
        .append(",\"source_timestamp_ns\":")
        .append(frame.sourceTimestampNs())
        .append(",\"inference_started_ns\":")
        .append(frame.inferenceStartedNs())
        .append(",\"inference_duration_ns\":")
        .append(frame.inferenceDurationNs())
        .append(",\"pose_count\":")
        .append(frame.poseCount())
        .append(",\"observation\":{\"person_confidence\":")
        .append(observation.personConfidence())
        .append(",\"address_confidence\":")
        .append(observation.addressConfidence())
        .append(",\"motion_magnitude\":")
        .append(observation.motionMagnitude())
        .append(",\"inside_hitting_region\":")
        .append(observation.insideHittingRegion())
        .append("},\"decision\":{\"state\":\"")
        .append(wire(decision.state()))
        .append("\",\"command\":\"")
        .append(wire(decision.command()))
        .append("\",\"transition_reason\":\"")
        .append(wire(decision.transitionReason()))
        .append("\",\"qualification_started_ns\":");
    appendOptionalLong(json, decision.qualificationStartedNs());
    json.append(",\"arm_requested_ns\":");
    appendOptionalLong(json, decision.armRequestedNs());
    json.append(",\"active_until_ns\":");
    appendOptionalLong(json, decision.activeUntilNs());
    json.append(",\"thermal_hard_stop_ns\":");
    appendOptionalLong(json, decision.thermalHardStopNs());
    json.append("}}");
  }

  private static FailureCode expectationFailure(
      PoseReplayConfiguration.Expectation expectation, int armRequests) {
    return switch (expectation) {
      case OBSERVE_ONLY -> FailureCode.NONE;
      case REQUIRE_ARM ->
          armRequests == 0 ? FailureCode.EXPECTED_ARM_NOT_OBSERVED : FailureCode.NONE;
      case REQUIRE_NO_ARM ->
          armRequests == 0 ? FailureCode.NONE : FailureCode.UNEXPECTED_ARM_REQUEST;
    };
  }

  private static int countArmRequests(List<FrameTrace> trace) {
    int count = 0;
    for (FrameTrace frame : trace) {
      if (frame.decision().command() == PoseTriggerController.Command.START_HIGH_SPEED) {
        count++;
      }
    }
    return count;
  }

  private static void appendOptionalLong(StringBuilder json, OptionalLong value) {
    if (value.isPresent()) {
      json.append(value.orElseThrow());
    } else {
      json.append("null");
    }
  }

  private static String wire(Enum<?> value) {
    return value.name().toLowerCase(Locale.ROOT);
  }

  private static String escape(String value) {
    return value.replace("\\", "\\\\").replace("\"", "\\\"");
  }
}
