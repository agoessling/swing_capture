package com.agoessling.swingcapture.pose.inference;

import com.agoessling.swingcapture.pose.PoseLandmarkFrame;
import com.agoessling.swingcapture.pose.PoseLandmarkObservationExtractor;
import com.agoessling.swingcapture.pose.PoseTriggerController;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

/** Pure stateful adapter that feeds inferred landmarks through the production trigger logic. */
public final class PoseReplayEvaluation {
  private final PoseReplayConfiguration configuration;
  private final PoseTriggerController controller;
  private final ArrayList<PoseReplayReport.FrameTrace> trace = new ArrayList<>();
  private PoseLandmarkFrame previous;

  public PoseReplayEvaluation(PoseReplayConfiguration configuration) {
    this.configuration = Objects.requireNonNull(configuration, "configuration");
    controller = new PoseTriggerController(configuration.controllerConfig());
  }

  public PoseReplayReport.FrameTrace accept(
      PoseLandmarkFrame landmarks, long inferenceStartedNs, long inferenceDurationNs) {
    Objects.requireNonNull(landmarks, "landmarks");
    if (trace.size() >= configuration.maximumFrames()) {
      throw new IllegalStateException("replay frame bound exceeded");
    }
    if (inferenceStartedNs < 0 || inferenceDurationNs < 0) {
      throw new IllegalArgumentException("inference timing cannot be negative");
    }
    PoseLandmarkObservationExtractor.Evaluation observation =
        PoseLandmarkObservationExtractor.evaluate(
            landmarks,
            previous,
            configuration.hittingRegion(),
            configuration.observationConfig(),
            configuration.projection());
    PoseTriggerController.Decision decision =
        controller.observe(observation.toControllerObservation());
    PoseReplayReport.FrameTrace frame =
        new PoseReplayReport.FrameTrace(
            trace.size(),
            landmarks.timestampNs(),
            inferenceStartedNs,
            inferenceDurationNs,
            landmarks.landmarks().isEmpty() ? 0 : 1,
            observation,
            decision);
    trace.add(frame);
    previous = landmarks;
    return frame;
  }

  public List<PoseReplayReport.FrameTrace> trace() {
    return List.copyOf(trace);
  }

  public PoseTriggerController.State finalState() {
    return controller.state();
  }
}
