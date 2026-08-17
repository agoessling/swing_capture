package com.agoessling.swingcapture.pose;

/** A two-dimensional landmark in normalized image coordinates with normalized visibility. */
public record NormalizedPoseLandmark(double x, double y, double visibility) {
  public NormalizedPoseLandmark {
    requireUnitInterval(x, "x");
    requireUnitInterval(y, "y");
    requireUnitInterval(visibility, "visibility");
  }

  public double distanceTo(NormalizedPoseLandmark other) {
    if (other == null) {
      throw new NullPointerException("other");
    }
    return Math.hypot(x - other.x, y - other.y);
  }

  private static void requireUnitInterval(double value, String name) {
    if (!Double.isFinite(value) || value < 0.0 || value > 1.0) {
      throw new IllegalArgumentException(name + " must be finite and in [0, 1]");
    }
  }
}
