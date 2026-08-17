package com.agoessling.swingcapture;

/** Ordered device-monotonic milestones for a warm low-rate to high-speed camera transition. */
public record WarmCaptureTransitionTiming(
    long transitionRequestedNs,
    long encoderStartedNs,
    long standbySessionClosedNs,
    long highSpeedSessionConfiguredNs,
    long firstHighSpeedCameraFrameNs,
    long firstUsableEncodedFrameNs) {
  public WarmCaptureTransitionTiming {
    requireNonnegative(transitionRequestedNs, "transitionRequestedNs");
    requireOrdered(transitionRequestedNs, encoderStartedNs, "encoderStartedNs");
    requireOrdered(encoderStartedNs, standbySessionClosedNs, "standbySessionClosedNs");
    requireOrdered(
        standbySessionClosedNs, highSpeedSessionConfiguredNs, "highSpeedSessionConfiguredNs");
    requireOrdered(
        highSpeedSessionConfiguredNs,
        firstHighSpeedCameraFrameNs,
        "firstHighSpeedCameraFrameNs");
    requireOrdered(
        firstHighSpeedCameraFrameNs,
        firstUsableEncodedFrameNs,
        "firstUsableEncodedFrameNs");
  }

  public long transitionToEncoderStartNs() {
    return Math.subtractExact(encoderStartedNs, transitionRequestedNs);
  }

  public long transitionToStandbySessionClosedNs() {
    return Math.subtractExact(standbySessionClosedNs, transitionRequestedNs);
  }

  public long transitionToHighSpeedSessionConfiguredNs() {
    return Math.subtractExact(highSpeedSessionConfiguredNs, transitionRequestedNs);
  }

  public long transitionToFirstHighSpeedCameraFrameNs() {
    return Math.subtractExact(firstHighSpeedCameraFrameNs, transitionRequestedNs);
  }

  public long transitionToFirstUsableEncodedFrameNs() {
    return Math.subtractExact(firstUsableEncodedFrameNs, transitionRequestedNs);
  }

  private static void requireNonnegative(long value, String name) {
    if (value < 0) {
      throw new IllegalArgumentException(name + " cannot be negative");
    }
  }

  private static void requireOrdered(long previous, long current, String name) {
    if (current < previous) {
      throw new IllegalArgumentException(name + " must not precede the previous milestone");
    }
  }
}
