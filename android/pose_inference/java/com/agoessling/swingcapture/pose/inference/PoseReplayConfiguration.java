package com.agoessling.swingcapture.pose.inference;

import com.agoessling.swingcapture.pose.NormalizedHittingRegion;
import com.agoessling.swingcapture.pose.PoseLandmarkObservationExtractor;
import com.agoessling.swingcapture.pose.PoseProjection;
import com.agoessling.swingcapture.pose.PoseTriggerController;
import java.util.Objects;

/** Immutable role, geometry, runtime, and expectation inputs for an on-device replay. */
public record PoseReplayConfiguration(
    String role,
    PoseProjection projection,
    NormalizedHittingRegion hittingRegion,
    PoseInferenceDelegatePolicy delegatePolicy,
    PoseLandmarkObservationExtractor.Config observationConfig,
    PoseTriggerController.Config controllerConfig,
    Expectation expectation,
    int maximumFrames) {
  public enum Expectation {
    OBSERVE_ONLY,
    REQUIRE_ARM,
    REQUIRE_NO_ARM
  }

  public static final int DEFAULT_MAXIMUM_FRAMES = 600;

  public PoseReplayConfiguration {
    Objects.requireNonNull(role, "role");
    Objects.requireNonNull(projection, "projection");
    Objects.requireNonNull(hittingRegion, "hittingRegion");
    Objects.requireNonNull(delegatePolicy, "delegatePolicy");
    Objects.requireNonNull(observationConfig, "observationConfig");
    Objects.requireNonNull(controllerConfig, "controllerConfig");
    Objects.requireNonNull(expectation, "expectation");
    PoseReplayReportValidator.validateRoleProjection(role, projection);
    if (maximumFrames <= 0 || maximumFrames > PoseReplayReportValidator.MAXIMUM_TRACE_FRAMES) {
      throw new IllegalArgumentException("maximumFrames is outside report bounds");
    }
  }

  public static PoseReplayConfiguration defaults(
      String role,
      PoseProjection projection,
      NormalizedHittingRegion hittingRegion,
      PoseInferenceDelegatePolicy delegatePolicy,
      Expectation expectation) {
    return new PoseReplayConfiguration(
        role,
        projection,
        hittingRegion,
        delegatePolicy,
        PoseLandmarkObservationExtractor.Config.defaultsForFiveFramesPerSecond(),
        PoseTriggerController.Config.defaultsForFiveFramesPerSecond(),
        expectation,
        DEFAULT_MAXIMUM_FRAMES);
  }
}
