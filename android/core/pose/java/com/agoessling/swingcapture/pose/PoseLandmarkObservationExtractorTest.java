package com.agoessling.swingcapture.pose;

import java.util.EnumMap;
import java.util.Map;

/** Deterministic geometry and integration tests for landmark-derived trigger observations. */
public final class PoseLandmarkObservationExtractorTest {
  private static final long MS = 1_000_000L;
  private static final PoseLandmarkObservationExtractor.Config CONFIG =
      PoseLandmarkObservationExtractor.Config.defaultsForFiveFramesPerSecond();
  private static final NormalizedHittingRegion HITTING_REGION =
      new NormalizedHittingRegion(0.25, 0.40, 0.75, 0.95);

  private PoseLandmarkObservationExtractorTest() {}

  public static void main(String[] arguments) {
    recognizesFaceOnAndDownTheLineAddressGeometry();
    downTheLinePolicyToleratesFarSideOcclusionOnly();
    feedsStableAddressToExistingController();
    usesFootSupportWithHipFallbackForHittingRegion();
    rejectsMissingAndLowVisibilityAddressJoints();
    walkingAndHighMotionDoNotQualify();
    enforcesFiniteRangeAndOrderingInvariants();
  }

  private static void recognizesFaceOnAndDownTheLineAddressGeometry() {
    PoseLandmarkObservationExtractor.Evaluation faceOn =
        evaluation(frame(0, faceOnAddress()), null);
    PoseLandmarkObservationExtractor.Evaluation downTheLine =
        evaluation(frame(0, downTheLineAddress()), null);

    check(faceOn.addressConfidence() > 0.75, "face-on address confidence");
    check(downTheLine.addressConfidence() > 0.75, "DTL address confidence");
    check(faceOn.insideHittingRegion(), "face-on region membership");
    check(downTheLine.insideHittingRegion(), "DTL region membership");
    check(faceOn.motionMagnitude() == 0.0, "first face-on frame motion");
    check(downTheLine.motionMagnitude() == 0.0, "first DTL frame motion");

    PoseLandmarkObservationExtractor.Evaluation standing =
        evaluation(frame(0, standingFaceOn()), null);
    check(standing.addressConfidence() < 0.40, "standing pose is not address");
  }

  private static void feedsStableAddressToExistingController() {
    PoseTriggerController faceOn = controller();
    PoseLandmarkFrame previous = null;
    for (int timestampMs = 0; timestampMs <= 400; timestampMs += 200) {
      PoseLandmarkFrame current = frame(timestampMs, faceOnAddress());
      PoseTriggerController.Command command =
          faceOn.observe(evaluation(current, previous).toControllerObservation()).command();
      if (timestampMs < 400) {
        check(command == PoseTriggerController.Command.NONE, "face-on qualification command");
      } else {
        check(
            command == PoseTriggerController.Command.START_HIGH_SPEED,
            "face-on address arms controller");
      }
      previous = current;
    }

    PoseTriggerController downTheLine = controller();
    previous = null;
    for (int timestampMs = 0; timestampMs <= 400; timestampMs += 200) {
      PoseLandmarkFrame current = frame(timestampMs, downTheLineAddress());
      PoseTriggerController.Command command =
          downTheLine.observe(evaluation(current, previous).toControllerObservation()).command();
      if (timestampMs == 400) {
        check(
            command == PoseTriggerController.Command.START_HIGH_SPEED,
            "DTL address arms controller");
      }
      previous = current;
    }
  }

  private static void downTheLinePolicyToleratesFarSideOcclusionOnly() {
    Map<PoseJoint, NormalizedPoseLandmark> occluded = copy(downTheLineAddress());
    occluded.put(PoseJoint.LEFT_WRIST, landmark(0.66, 0.58, 0.20));
    occluded.put(PoseJoint.LEFT_KNEE, landmark(0.51, 0.68, 0.30));
    PoseLandmarkFrame frame = frame(0, occluded);

    PoseLandmarkObservationExtractor.Evaluation acrossTheLine = evaluation(frame, null);
    PoseLandmarkObservationExtractor.Evaluation downTheLine =
        PoseLandmarkObservationExtractor.evaluate(
            frame, null, HITTING_REGION, CONFIG, PoseProjection.DOWN_THE_LINE);
    check(acrossTheLine.addressConfidence() == 0.0, "ATL requires bilateral visibility");
    check(downTheLine.personConfidence() >= 0.90, "DTL confidence uses the visible side");
    check(downTheLine.addressConfidence() >= 0.45, "DTL accepts far-side occlusion");

    occluded.put(PoseJoint.RIGHT_WRIST, landmark(0.68, 0.58, 0.20));
    check(
        PoseLandmarkObservationExtractor.addressConfidence(
                frame(0, occluded), CONFIG, PoseProjection.DOWN_THE_LINE)
            == 0.0,
        "DTL still requires one visible wrist");
  }

  private static void usesFootSupportWithHipFallbackForHittingRegion() {
    PoseLandmarkFrame inside = frame(0, faceOnAddress());
    check(HITTING_REGION.containsSupportPoint(inside, 0.50), "ankle support point inside");

    Map<PoseJoint, NormalizedPoseLandmark> withoutAnkles = copy(faceOnAddress());
    withoutAnkles.remove(PoseJoint.LEFT_ANKLE);
    withoutAnkles.remove(PoseJoint.RIGHT_ANKLE);
    PoseLandmarkFrame hipFallback = frame(0, withoutAnkles);
    check(HITTING_REGION.containsSupportPoint(hipFallback, 0.50), "hip support fallback");

    Map<PoseJoint, NormalizedPoseLandmark> outside = shifted(faceOnAddress(), -0.35, 0.0);
    check(
        !HITTING_REGION.containsSupportPoint(frame(0, outside), 0.50),
        "outside support point rejected");
  }

  private static void rejectsMissingAndLowVisibilityAddressJoints() {
    Map<PoseJoint, NormalizedPoseLandmark> missing = copy(faceOnAddress());
    missing.remove(PoseJoint.LEFT_WRIST);
    PoseLandmarkObservationExtractor.Evaluation missingEvaluation =
        evaluation(frame(0, missing), null);
    check(missingEvaluation.addressConfidence() == 0.0, "missing wrist rejects address");

    Map<PoseJoint, NormalizedPoseLandmark> lowVisibility = copy(faceOnAddress());
    NormalizedPoseLandmark wrist = lowVisibility.get(PoseJoint.RIGHT_WRIST);
    lowVisibility.put(
        PoseJoint.RIGHT_WRIST,
        new NormalizedPoseLandmark(wrist.x(), wrist.y(), CONFIG.minimumVisibility() - 0.01));
    PoseLandmarkObservationExtractor.Evaluation lowVisibilityEvaluation =
        evaluation(frame(0, lowVisibility), null);
    check(
        lowVisibilityEvaluation.addressConfidence() == 0.0,
        "low-visibility wrist rejects address");

    Map<PoseJoint, NormalizedPoseLandmark> sparse = new EnumMap<>(PoseJoint.class);
    sparse.put(PoseJoint.LEFT_HIP, landmark(0.45, 0.50));
    sparse.put(PoseJoint.RIGHT_HIP, landmark(0.55, 0.50));
    PoseLandmarkObservationExtractor.Evaluation sparseEvaluation =
        evaluation(frame(0, sparse), null);
    check(sparseEvaluation.personConfidence() < 0.25, "missing joints lower person confidence");
  }

  private static void walkingAndHighMotionDoNotQualify() {
    PoseLandmarkFrame first = frame(0, walkingPose());
    PoseLandmarkFrame second = frame(200, shifted(walkingPose(), 0.06, 0.0));
    PoseLandmarkObservationExtractor.Evaluation walking = evaluation(second, first);
    check(walking.addressConfidence() < 0.60, "walking geometry rejects address");
    check(walking.motionMagnitude() > 0.18, "walking displacement exceeds controller limit");

    PoseLandmarkFrame addressFirst = frame(0, faceOnAddress());
    PoseLandmarkFrame addressMoved = frame(200, shifted(faceOnAddress(), 0.06, 0.0));
    PoseLandmarkObservationExtractor.Evaluation movingAddress =
        evaluation(addressMoved, addressFirst);
    check(movingAddress.addressConfidence() > 0.75, "translated address retains geometry");
    check(movingAddress.motionMagnitude() > 0.18, "translated address exposes high motion");

    PoseTriggerController controller = controller();
    controller.observe(evaluation(first, null).toControllerObservation());
    controller.observe(walking.toControllerObservation());
    check(controller.state() == PoseTriggerController.State.WATCHING, "walking does not qualify");
  }

  private static void enforcesFiniteRangeAndOrderingInvariants() {
    expectThrows(
        IllegalArgumentException.class,
        () -> new NormalizedPoseLandmark(Double.NaN, 0.5, 1.0),
        "nonfinite landmark");
    expectThrows(
        IllegalArgumentException.class,
        () -> new NormalizedPoseLandmark(1.01, 0.5, 1.0),
        "landmark outside normalized image");
    expectThrows(
        IllegalArgumentException.class,
        () -> new NormalizedHittingRegion(0.5, 0.1, 0.5, 0.9),
        "empty hitting region");
    expectThrows(
        IllegalArgumentException.class,
        () -> frame(-1, faceOnAddress()),
        "negative frame timestamp");
    expectThrows(
        IllegalArgumentException.class,
        () -> evaluation(frame(200, faceOnAddress()), frame(200, faceOnAddress())),
        "unordered motion frames");
    expectThrows(
        IllegalArgumentException.class,
        () -> new PoseLandmarkObservationExtractor.Config(0.5, 0, 0.35),
        "zero matched landmark threshold");

    PoseLandmarkFrame previous = frame(0, faceOnAddress());
    for (int index = 1; index <= 10; ++index) {
      double shift = index * 0.002;
      PoseLandmarkFrame current = frame(index * 200, shifted(faceOnAddress(), shift, 0.0));
      PoseLandmarkObservationExtractor.Evaluation evaluation = evaluation(current, previous);
      checkUnit(evaluation.personConfidence(), "person confidence invariant");
      checkUnit(evaluation.addressConfidence(), "address confidence invariant");
      checkUnit(evaluation.motionMagnitude(), "motion invariant");
      previous = current;
    }
  }

  private static PoseLandmarkObservationExtractor.Evaluation evaluation(
      PoseLandmarkFrame current, PoseLandmarkFrame previous) {
    return PoseLandmarkObservationExtractor.evaluate(current, previous, HITTING_REGION, CONFIG);
  }

  private static PoseTriggerController controller() {
    return new PoseTriggerController(PoseTriggerController.Config.defaultsForFiveFramesPerSecond());
  }

  private static PoseLandmarkFrame frame(
      long timestampMs, Map<PoseJoint, NormalizedPoseLandmark> landmarks) {
    return new PoseLandmarkFrame(timestampMs * MS, 0.95, landmarks);
  }

  private static Map<PoseJoint, NormalizedPoseLandmark> faceOnAddress() {
    EnumMap<PoseJoint, NormalizedPoseLandmark> pose = new EnumMap<>(PoseJoint.class);
    pose.put(PoseJoint.LEFT_SHOULDER, landmark(0.42, 0.30));
    pose.put(PoseJoint.RIGHT_SHOULDER, landmark(0.58, 0.30));
    pose.put(PoseJoint.LEFT_ELBOW, landmark(0.44, 0.44));
    pose.put(PoseJoint.RIGHT_ELBOW, landmark(0.56, 0.44));
    pose.put(PoseJoint.LEFT_WRIST, landmark(0.49, 0.59));
    pose.put(PoseJoint.RIGHT_WRIST, landmark(0.51, 0.59));
    pose.put(PoseJoint.LEFT_HIP, landmark(0.44, 0.51));
    pose.put(PoseJoint.RIGHT_HIP, landmark(0.56, 0.51));
    pose.put(PoseJoint.LEFT_KNEE, landmark(0.42, 0.69));
    pose.put(PoseJoint.RIGHT_KNEE, landmark(0.58, 0.69));
    pose.put(PoseJoint.LEFT_ANKLE, landmark(0.36, 0.88));
    pose.put(PoseJoint.RIGHT_ANKLE, landmark(0.64, 0.88));
    return pose;
  }

  private static Map<PoseJoint, NormalizedPoseLandmark> downTheLineAddress() {
    EnumMap<PoseJoint, NormalizedPoseLandmark> pose = new EnumMap<>(PoseJoint.class);
    pose.put(PoseJoint.LEFT_SHOULDER, landmark(0.50, 0.30));
    pose.put(PoseJoint.RIGHT_SHOULDER, landmark(0.53, 0.30));
    pose.put(PoseJoint.LEFT_ELBOW, landmark(0.59, 0.43));
    pose.put(PoseJoint.RIGHT_ELBOW, landmark(0.61, 0.43));
    pose.put(PoseJoint.LEFT_WRIST, landmark(0.66, 0.58));
    pose.put(PoseJoint.RIGHT_WRIST, landmark(0.68, 0.58));
    pose.put(PoseJoint.LEFT_HIP, landmark(0.58, 0.50));
    pose.put(PoseJoint.RIGHT_HIP, landmark(0.61, 0.50));
    pose.put(PoseJoint.LEFT_KNEE, landmark(0.51, 0.68));
    pose.put(PoseJoint.RIGHT_KNEE, landmark(0.54, 0.68));
    pose.put(PoseJoint.LEFT_ANKLE, landmark(0.54, 0.88));
    pose.put(PoseJoint.RIGHT_ANKLE, landmark(0.57, 0.88));
    return pose;
  }

  private static Map<PoseJoint, NormalizedPoseLandmark> standingFaceOn() {
    EnumMap<PoseJoint, NormalizedPoseLandmark> pose = new EnumMap<>(PoseJoint.class);
    pose.put(PoseJoint.LEFT_SHOULDER, landmark(0.42, 0.25));
    pose.put(PoseJoint.RIGHT_SHOULDER, landmark(0.58, 0.25));
    pose.put(PoseJoint.LEFT_ELBOW, landmark(0.39, 0.38));
    pose.put(PoseJoint.RIGHT_ELBOW, landmark(0.61, 0.38));
    pose.put(PoseJoint.LEFT_WRIST, landmark(0.37, 0.51));
    pose.put(PoseJoint.RIGHT_WRIST, landmark(0.63, 0.51));
    pose.put(PoseJoint.LEFT_HIP, landmark(0.44, 0.50));
    pose.put(PoseJoint.RIGHT_HIP, landmark(0.56, 0.50));
    pose.put(PoseJoint.LEFT_KNEE, landmark(0.44, 0.69));
    pose.put(PoseJoint.RIGHT_KNEE, landmark(0.56, 0.69));
    pose.put(PoseJoint.LEFT_ANKLE, landmark(0.43, 0.89));
    pose.put(PoseJoint.RIGHT_ANKLE, landmark(0.57, 0.89));
    return pose;
  }

  private static Map<PoseJoint, NormalizedPoseLandmark> walkingPose() {
    Map<PoseJoint, NormalizedPoseLandmark> pose = copy(standingFaceOn());
    pose.put(PoseJoint.LEFT_WRIST, landmark(0.32, 0.45));
    pose.put(PoseJoint.RIGHT_WRIST, landmark(0.65, 0.36));
    pose.put(PoseJoint.LEFT_KNEE, landmark(0.39, 0.68));
    pose.put(PoseJoint.RIGHT_KNEE, landmark(0.60, 0.66));
    pose.put(PoseJoint.LEFT_ANKLE, landmark(0.34, 0.88));
    pose.put(PoseJoint.RIGHT_ANKLE, landmark(0.65, 0.84));
    return pose;
  }

  private static Map<PoseJoint, NormalizedPoseLandmark> shifted(
      Map<PoseJoint, NormalizedPoseLandmark> source, double xOffset, double yOffset) {
    EnumMap<PoseJoint, NormalizedPoseLandmark> shifted = new EnumMap<>(PoseJoint.class);
    for (Map.Entry<PoseJoint, NormalizedPoseLandmark> entry : source.entrySet()) {
      NormalizedPoseLandmark point = entry.getValue();
      shifted.put(
          entry.getKey(),
          new NormalizedPoseLandmark(
              point.x() + xOffset, point.y() + yOffset, point.visibility()));
    }
    return shifted;
  }

  private static Map<PoseJoint, NormalizedPoseLandmark> copy(
      Map<PoseJoint, NormalizedPoseLandmark> source) {
    return new EnumMap<>(source);
  }

  private static NormalizedPoseLandmark landmark(double x, double y) {
    return landmark(x, y, 0.95);
  }

  private static NormalizedPoseLandmark landmark(double x, double y, double visibility) {
    return new NormalizedPoseLandmark(x, y, visibility);
  }

  private static void checkUnit(double value, String message) {
    check(Double.isFinite(value) && value >= 0.0 && value <= 1.0, message);
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
