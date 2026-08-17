package com.agoessling.swingcapture;

/** Ordered device-monotonic milestones for one transition into high-speed capture. */
public record CaptureStartupTiming(
    long armRequestedNs,
    long engineStartedNs,
    long firstCameraFrameNs,
    long firstUsableEncodedFrameNs,
    long fullPreRollReadyNs) {
  public CaptureStartupTiming {
    requireNonnegative(armRequestedNs, "armRequestedNs");
    requireOrdered(armRequestedNs, engineStartedNs, "engineStartedNs");
    requireOrdered(engineStartedNs, firstCameraFrameNs, "firstCameraFrameNs");
    requireOrdered(firstCameraFrameNs, firstUsableEncodedFrameNs, "firstUsableEncodedFrameNs");
    requireOrdered(firstUsableEncodedFrameNs, fullPreRollReadyNs, "fullPreRollReadyNs");
  }

  public long armToEngineStartNs() {
    return Math.subtractExact(engineStartedNs, armRequestedNs);
  }

  public long armToFirstCameraFrameNs() {
    return Math.subtractExact(firstCameraFrameNs, armRequestedNs);
  }

  public long armToFirstUsableEncodedFrameNs() {
    return Math.subtractExact(firstUsableEncodedFrameNs, armRequestedNs);
  }

  public long armToFullPreRollReadyNs() {
    return Math.subtractExact(fullPreRollReadyNs, armRequestedNs);
  }

  public long firstUsableEncodedFrameToFullPreRollReadyNs() {
    return Math.subtractExact(fullPreRollReadyNs, firstUsableEncodedFrameNs);
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
