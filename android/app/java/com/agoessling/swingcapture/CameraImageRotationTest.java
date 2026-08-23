package com.agoessling.swingcapture;

/** Regression coverage for retaining the selected camera's sensor orientation. */
public final class CameraImageRotationTest {
  private CameraImageRotationTest() {}

  public static void main(String[] arguments) {
    check(CameraImageRotation.fromSensorOrientation(0) == 0, "zero-degree sensor");
    check(CameraImageRotation.fromSensorOrientation(90) == 90, "portrait Pixel rear sensor");
    check(CameraImageRotation.fromSensorOrientation(180) == 180, "inverted sensor");
    check(CameraImageRotation.fromSensorOrientation(270) == 270, "counter-rotated sensor");
    expectIllegalArgument(() -> CameraImageRotation.fromSensorOrientation(-90));
    expectIllegalArgument(() -> CameraImageRotation.fromSensorOrientation(45));
    expectIllegalArgument(() -> CameraImageRotation.fromSensorOrientation(360));
  }

  private static void expectIllegalArgument(Runnable action) {
    try {
      action.run();
    } catch (IllegalArgumentException expected) {
      return;
    }
    throw new AssertionError("unsupported camera sensor rotation was accepted");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
