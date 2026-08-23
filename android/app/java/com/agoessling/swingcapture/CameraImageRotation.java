package com.agoessling.swingcapture;

/** Pure validation for Camera2 sensor rotations passed through to MediaPipe image processing. */
public final class CameraImageRotation {
  private CameraImageRotation() {}

  /** Returns an exact Camera2 sensor orientation when MediaPipe can consume it. */
  public static int fromSensorOrientation(int sensorOrientationDegrees) {
    if (sensorOrientationDegrees != 0
        && sensorOrientationDegrees != 90
        && sensorOrientationDegrees != 180
        && sensorOrientationDegrees != 270) {
      throw new IllegalArgumentException("sensor orientation must be 0, 90, 180, or 270");
    }
    return sensorOrientationDegrees;
  }
}
