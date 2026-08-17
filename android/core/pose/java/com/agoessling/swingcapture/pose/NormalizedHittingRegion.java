package com.agoessling.swingcapture.pose;

import java.util.Objects;

/** Inclusive normalized image rectangle representing the configured hitting station. */
public record NormalizedHittingRegion(double left, double top, double right, double bottom) {
  public NormalizedHittingRegion {
    requireUnitInterval(left, "left");
    requireUnitInterval(top, "top");
    requireUnitInterval(right, "right");
    requireUnitInterval(bottom, "bottom");
    if (right <= left || bottom <= top) {
      throw new IllegalArgumentException("hitting region must have positive width and height");
    }
  }

  public boolean contains(NormalizedPoseLandmark landmark) {
    Objects.requireNonNull(landmark, "landmark");
    return contains(landmark.x(), landmark.y());
  }

  public boolean contains(double x, double y) {
    if (!Double.isFinite(x) || !Double.isFinite(y)) {
      throw new IllegalArgumentException("point must be finite");
    }
    return x >= left && x <= right && y >= top && y <= bottom;
  }

  /**
   * Tests the midpoint between visible ankles, falling back to the hip midpoint when feet are not
   * available. A single visible side is insufficient to claim station occupancy.
   */
  public boolean containsSupportPoint(PoseLandmarkFrame frame, double minimumVisibility) {
    Objects.requireNonNull(frame, "frame");
    requireUnitInterval(minimumVisibility, "minimumVisibility");

    Point support =
        visibleMidpoint(frame, PoseJoint.LEFT_ANKLE, PoseJoint.RIGHT_ANKLE, minimumVisibility);
    if (support == null) {
      support = visibleMidpoint(frame, PoseJoint.LEFT_HIP, PoseJoint.RIGHT_HIP, minimumVisibility);
    }
    return support != null && contains(support.x(), support.y());
  }

  private static Point visibleMidpoint(
      PoseLandmarkFrame frame,
      PoseJoint left,
      PoseJoint right,
      double minimumVisibility) {
    NormalizedPoseLandmark leftPoint = frame.landmarks().get(left);
    NormalizedPoseLandmark rightPoint = frame.landmarks().get(right);
    if (!visible(leftPoint, minimumVisibility) || !visible(rightPoint, minimumVisibility)) {
      return null;
    }
    return new Point((leftPoint.x() + rightPoint.x()) / 2.0, (leftPoint.y() + rightPoint.y()) / 2.0);
  }

  private static boolean visible(NormalizedPoseLandmark landmark, double minimumVisibility) {
    return landmark != null && landmark.visibility() >= minimumVisibility;
  }

  private static void requireUnitInterval(double value, String name) {
    if (!Double.isFinite(value) || value < 0.0 || value > 1.0) {
      throw new IllegalArgumentException(name + " must be finite and in [0, 1]");
    }
  }

  private record Point(double x, double y) {}
}
