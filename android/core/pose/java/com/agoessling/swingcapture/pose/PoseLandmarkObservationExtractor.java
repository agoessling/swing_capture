package com.agoessling.swingcapture.pose;

import java.util.Objects;

/** Converts normalized landmarks into the model-independent pose trigger observation contract. */
public final class PoseLandmarkObservationExtractor {
  private static final PoseJoint[] ADDRESS_JOINTS = {
    PoseJoint.LEFT_SHOULDER,
    PoseJoint.RIGHT_SHOULDER,
    PoseJoint.LEFT_WRIST,
    PoseJoint.RIGHT_WRIST,
    PoseJoint.LEFT_HIP,
    PoseJoint.RIGHT_HIP,
    PoseJoint.LEFT_KNEE,
    PoseJoint.RIGHT_KNEE,
    PoseJoint.LEFT_ANKLE,
    PoseJoint.RIGHT_ANKLE
  };
  private static final double MINIMUM_BODY_SCALE = 1.0e-6;

  /** Feature thresholds, independent of the controller's temporal hysteresis thresholds. */
  public record Config(
      double minimumVisibility,
      int minimumMatchedMotionLandmarks,
      double motionSaturationBodyLengths) {
    public Config {
      requireUnitInterval(minimumVisibility, "minimumVisibility");
      if (minimumMatchedMotionLandmarks <= 0
          || minimumMatchedMotionLandmarks > PoseJoint.values().length) {
        throw new IllegalArgumentException(
            "minimumMatchedMotionLandmarks must fit the supported joint set");
      }
      if (!Double.isFinite(motionSaturationBodyLengths)
          || motionSaturationBodyLengths <= 0.0) {
        throw new IllegalArgumentException("motionSaturationBodyLengths must be positive");
      }
    }

    public static Config defaultsForFiveFramesPerSecond() {
      return new Config(0.50, 6, 0.35);
    }
  }

  /** Auditable scalar features plus a direct adapter to the existing controller contract. */
  public record Evaluation(
      long timestampNs,
      double personConfidence,
      double addressConfidence,
      double motionMagnitude,
      boolean insideHittingRegion) {
    public Evaluation {
      if (timestampNs < 0) {
        throw new IllegalArgumentException("timestampNs cannot be negative");
      }
      requireUnitInterval(personConfidence, "personConfidence");
      requireUnitInterval(addressConfidence, "addressConfidence");
      requireUnitInterval(motionMagnitude, "motionMagnitude");
    }

    public PoseTriggerController.Observation toControllerObservation() {
      return new PoseTriggerController.Observation(
          timestampNs,
          personConfidence,
          addressConfidence,
          motionMagnitude,
          insideHittingRegion);
    }
  }

  private PoseLandmarkObservationExtractor() {}

  public static Evaluation evaluate(
      PoseLandmarkFrame current,
      PoseLandmarkFrame previous,
      NormalizedHittingRegion hittingRegion,
      Config config) {
    return evaluate(current, previous, hittingRegion, config, PoseProjection.ACROSS_THE_LINE);
  }

  public static Evaluation evaluate(
      PoseLandmarkFrame current,
      PoseLandmarkFrame previous,
      NormalizedHittingRegion hittingRegion,
      Config config,
      PoseProjection projection) {
    Objects.requireNonNull(current, "current");
    Objects.requireNonNull(hittingRegion, "hittingRegion");
    Objects.requireNonNull(config, "config");
    Objects.requireNonNull(projection, "projection");
    if (previous != null && previous.timestampNs() >= current.timestampNs()) {
      throw new IllegalArgumentException("previous frame must precede current frame");
    }

    return new Evaluation(
        current.timestampNs(),
        effectivePersonConfidence(current, config, projection),
        addressConfidence(current, config, projection),
        motionMagnitude(current, previous, config, projection),
        hittingRegion.containsSupportPoint(current, config.minimumVisibility()));
  }

  public static double effectivePersonConfidence(PoseLandmarkFrame frame, Config config) {
    return effectivePersonConfidence(frame, config, PoseProjection.ACROSS_THE_LINE);
  }

  public static double effectivePersonConfidence(
      PoseLandmarkFrame frame, Config config, PoseProjection projection) {
    Objects.requireNonNull(frame, "frame");
    Objects.requireNonNull(config, "config");
    Objects.requireNonNull(projection, "projection");
    if (projection == PoseProjection.DOWN_THE_LINE) {
      PoseJoint[][] pairs = {
        {PoseJoint.LEFT_SHOULDER, PoseJoint.RIGHT_SHOULDER},
        {PoseJoint.LEFT_WRIST, PoseJoint.RIGHT_WRIST},
        {PoseJoint.LEFT_HIP, PoseJoint.RIGHT_HIP},
        {PoseJoint.LEFT_KNEE, PoseJoint.RIGHT_KNEE},
        {PoseJoint.LEFT_ANKLE, PoseJoint.RIGHT_ANKLE}
      };
      double visibilityEvidence = 0.0;
      for (PoseJoint[] pair : pairs) {
        visibilityEvidence +=
            Math.max(
                visibility(frame.landmarks().get(pair[0])),
                visibility(frame.landmarks().get(pair[1])));
      }
      return Math.min(frame.personConfidence(), visibilityEvidence / pairs.length);
    }
    double visibilityEvidence = 0.0;
    for (PoseJoint joint : ADDRESS_JOINTS) {
      NormalizedPoseLandmark landmark = frame.landmarks().get(joint);
      if (landmark != null) {
        visibilityEvidence += landmark.visibility();
      }
    }
    visibilityEvidence /= ADDRESS_JOINTS.length;
    return Math.min(frame.personConfidence(), visibilityEvidence);
  }

  /**
   * Geometry-only address likelihood. Stillness and station occupancy intentionally remain
   * separate controller inputs.
   */
  public static double addressConfidence(PoseLandmarkFrame frame, Config config) {
    return addressConfidence(frame, config, PoseProjection.ACROSS_THE_LINE);
  }

  public static double addressConfidence(
      PoseLandmarkFrame frame, Config config, PoseProjection projection) {
    Objects.requireNonNull(frame, "frame");
    Objects.requireNonNull(config, "config");
    Objects.requireNonNull(projection, "projection");
    if (projection == PoseProjection.ACROSS_THE_LINE) {
      for (PoseJoint joint : ADDRESS_JOINTS) {
        if (!visible(frame.landmarks().get(joint), config.minimumVisibility())) {
          return 0.0;
        }
      }
    }

    NormalizedPoseLandmark leftShoulder = frame.landmarks().get(PoseJoint.LEFT_SHOULDER);
    NormalizedPoseLandmark rightShoulder = frame.landmarks().get(PoseJoint.RIGHT_SHOULDER);
    NormalizedPoseLandmark leftWrist = frame.landmarks().get(PoseJoint.LEFT_WRIST);
    NormalizedPoseLandmark rightWrist = frame.landmarks().get(PoseJoint.RIGHT_WRIST);
    NormalizedPoseLandmark leftHip = frame.landmarks().get(PoseJoint.LEFT_HIP);
    NormalizedPoseLandmark rightHip = frame.landmarks().get(PoseJoint.RIGHT_HIP);
    NormalizedPoseLandmark leftKnee = frame.landmarks().get(PoseJoint.LEFT_KNEE);
    NormalizedPoseLandmark rightKnee = frame.landmarks().get(PoseJoint.RIGHT_KNEE);
    NormalizedPoseLandmark leftAnkle = frame.landmarks().get(PoseJoint.LEFT_ANKLE);
    NormalizedPoseLandmark rightAnkle = frame.landmarks().get(PoseJoint.RIGHT_ANKLE);

    double bodyScale = bodyScale(frame, config.minimumVisibility(), projection);
    if (bodyScale < MINIMUM_BODY_SCALE) {
      return 0.0;
    }

    Point shoulderCenter =
        representative(leftShoulder, rightShoulder, config.minimumVisibility());
    Point wristCenter = representative(leftWrist, rightWrist, config.minimumVisibility());
    Point hipCenter = representative(leftHip, rightHip, config.minimumVisibility());
    Point kneeCenter = representative(leftKnee, rightKnee, config.minimumVisibility());
    Point ankleCenter = representative(leftAnkle, rightAnkle, config.minimumVisibility());
    if (shoulderCenter == null
        || wristCenter == null
        || hipCenter == null
        || kneeCenter == null
        || ankleCenter == null) {
      return 0.0;
    }

    double handDrop = ramp((wristCenter.y() - hipCenter.y()) / bodyScale, 0.08, 0.28);
    double gripTogether =
        visible(leftWrist, config.minimumVisibility())
                && visible(rightWrist, config.minimumVisibility())
            ? 1.0 - ramp(leftWrist.distanceTo(rightWrist) / bodyScale, 0.22, 0.75)
            : 0.75;
    double handReach =
        1.0 - ramp(Math.abs(wristCenter.x() - hipCenter.x()) / bodyScale, 0.80, 1.60);
    double gripGeometry = Math.cbrt(handDrop * gripTogether * handReach);

    double kneeFlex = 0.0;
    int kneeEvidenceCount = 0;
    if (visible(leftHip, config.minimumVisibility())
        && visible(leftKnee, config.minimumVisibility())
        && visible(leftAnkle, config.minimumVisibility())) {
      kneeFlex +=
          rangeScore(jointAngleDegrees(leftHip, leftKnee, leftAnkle), 105.0, 135.0, 164.0, 179.0);
      ++kneeEvidenceCount;
    }
    if (visible(rightHip, config.minimumVisibility())
        && visible(rightKnee, config.minimumVisibility())
        && visible(rightAnkle, config.minimumVisibility())) {
      kneeFlex +=
          rangeScore(
              jointAngleDegrees(rightHip, rightKnee, rightAnkle), 105.0, 135.0, 164.0, 179.0);
      ++kneeEvidenceCount;
    }
    if (kneeEvidenceCount == 0) {
      return 0.0;
    }
    kneeFlex /= kneeEvidenceCount;

    double stance =
        visible(leftAnkle, config.minimumVisibility())
                && visible(rightAnkle, config.minimumVisibility())
            ? rangeScore(leftAnkle.distanceTo(rightAnkle) / bodyScale, 0.04, 0.12, 1.50, 2.20)
            : 0.50;
    double verticalOrder =
        Math.min(
            ramp((hipCenter.y() - shoulderCenter.y()) / bodyScale, 0.10, 0.35),
            Math.min(
                ramp((kneeCenter.y() - hipCenter.y()) / bodyScale, 0.10, 0.35),
                ramp((ankleCenter.y() - kneeCenter.y()) / bodyScale, 0.10, 0.35)));

    return clampUnit(0.50 * gripGeometry + 0.25 * kneeFlex + 0.10 * stance + 0.15 * verticalOrder);
  }

  /** Mean landmark displacement normalized by torso length, saturated into [0, 1]. */
  public static double motionMagnitude(
      PoseLandmarkFrame current, PoseLandmarkFrame previous, Config config) {
    return motionMagnitude(current, previous, config, PoseProjection.ACROSS_THE_LINE);
  }

  public static double motionMagnitude(
      PoseLandmarkFrame current,
      PoseLandmarkFrame previous,
      Config config,
      PoseProjection projection) {
    Objects.requireNonNull(current, "current");
    Objects.requireNonNull(config, "config");
    Objects.requireNonNull(projection, "projection");
    if (previous == null) {
      return 0.0;
    }
    if (previous.timestampNs() >= current.timestampNs()) {
      throw new IllegalArgumentException("previous frame must precede current frame");
    }

    double currentScale = bodyScale(current, config.minimumVisibility(), projection);
    double previousScale = bodyScale(previous, config.minimumVisibility(), projection);
    double scale = (currentScale + previousScale) / 2.0;
    if (scale < MINIMUM_BODY_SCALE) {
      return 1.0;
    }

    double squaredNormalizedDisplacement = 0.0;
    int matched = 0;
    for (PoseJoint joint : PoseJoint.values()) {
      NormalizedPoseLandmark currentPoint = current.landmarks().get(joint);
      NormalizedPoseLandmark previousPoint = previous.landmarks().get(joint);
      if (visible(currentPoint, config.minimumVisibility())
          && visible(previousPoint, config.minimumVisibility())) {
        double displacement = currentPoint.distanceTo(previousPoint) / scale;
        squaredNormalizedDisplacement += displacement * displacement;
        ++matched;
      }
    }
    if (matched < config.minimumMatchedMotionLandmarks()) {
      return 1.0;
    }
    double rootMeanSquare = Math.sqrt(squaredNormalizedDisplacement / matched);
    return clampUnit(rootMeanSquare / config.motionSaturationBodyLengths());
  }

  private static double bodyScale(
      PoseLandmarkFrame frame, double minimumVisibility, PoseProjection projection) {
    NormalizedPoseLandmark leftShoulder = frame.landmarks().get(PoseJoint.LEFT_SHOULDER);
    NormalizedPoseLandmark rightShoulder = frame.landmarks().get(PoseJoint.RIGHT_SHOULDER);
    NormalizedPoseLandmark leftHip = frame.landmarks().get(PoseJoint.LEFT_HIP);
    NormalizedPoseLandmark rightHip = frame.landmarks().get(PoseJoint.RIGHT_HIP);
    if (projection == PoseProjection.ACROSS_THE_LINE) {
      if (!visible(leftShoulder, minimumVisibility)
          || !visible(rightShoulder, minimumVisibility)
          || !visible(leftHip, minimumVisibility)
          || !visible(rightHip, minimumVisibility)) {
        return 0.0;
      }
      return (leftShoulder.distanceTo(leftHip) + rightShoulder.distanceTo(rightHip)) / 2.0;
    }
    double scale = 0.0;
    int evidenceCount = 0;
    if (visible(leftShoulder, minimumVisibility) && visible(leftHip, minimumVisibility)) {
      scale += leftShoulder.distanceTo(leftHip);
      ++evidenceCount;
    }
    if (visible(rightShoulder, minimumVisibility) && visible(rightHip, minimumVisibility)) {
      scale += rightShoulder.distanceTo(rightHip);
      ++evidenceCount;
    }
    return evidenceCount == 0 ? 0.0 : scale / evidenceCount;
  }

  private static double jointAngleDegrees(
      NormalizedPoseLandmark first,
      NormalizedPoseLandmark vertex,
      NormalizedPoseLandmark third) {
    double firstX = first.x() - vertex.x();
    double firstY = first.y() - vertex.y();
    double thirdX = third.x() - vertex.x();
    double thirdY = third.y() - vertex.y();
    double denominator = Math.hypot(firstX, firstY) * Math.hypot(thirdX, thirdY);
    if (denominator < MINIMUM_BODY_SCALE) {
      return 180.0;
    }
    double cosine = (firstX * thirdX + firstY * thirdY) / denominator;
    return Math.toDegrees(Math.acos(Math.max(-1.0, Math.min(1.0, cosine))));
  }

  private static double rangeScore(
      double value, double outerLow, double innerLow, double innerHigh, double outerHigh) {
    if (value <= outerLow || value >= outerHigh) {
      return 0.0;
    }
    if (value < innerLow) {
      return ramp(value, outerLow, innerLow);
    }
    if (value <= innerHigh) {
      return 1.0;
    }
    return 1.0 - ramp(value, innerHigh, outerHigh);
  }

  private static double ramp(double value, double low, double high) {
    if (value <= low) {
      return 0.0;
    }
    if (value >= high) {
      return 1.0;
    }
    return (value - low) / (high - low);
  }

  private static double clampUnit(double value) {
    return Math.max(0.0, Math.min(1.0, value));
  }

  private static Point midpoint(
      NormalizedPoseLandmark first, NormalizedPoseLandmark second) {
    return new Point((first.x() + second.x()) / 2.0, (first.y() + second.y()) / 2.0);
  }

  private static Point representative(
      NormalizedPoseLandmark first,
      NormalizedPoseLandmark second,
      double minimumVisibility) {
    boolean firstVisible = visible(first, minimumVisibility);
    boolean secondVisible = visible(second, minimumVisibility);
    if (firstVisible && secondVisible) {
      return midpoint(first, second);
    }
    if (firstVisible) {
      return new Point(first.x(), first.y());
    }
    if (secondVisible) {
      return new Point(second.x(), second.y());
    }
    return null;
  }

  private static boolean visible(NormalizedPoseLandmark landmark, double minimumVisibility) {
    return landmark != null && landmark.visibility() >= minimumVisibility;
  }

  private static double visibility(NormalizedPoseLandmark landmark) {
    return landmark == null ? 0.0 : landmark.visibility();
  }

  private static void requireUnitInterval(double value, String name) {
    if (!Double.isFinite(value) || value < 0.0 || value > 1.0) {
      throw new IllegalArgumentException(name + " must be finite and in [0, 1]");
    }
  }

  private record Point(double x, double y) {}
}
