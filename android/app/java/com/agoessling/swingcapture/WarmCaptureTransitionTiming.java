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

  /** Camera2 reports FLUSHED for an in-flight standby request during an intentional abort. */
  public static boolean standbyCaptureFailureIsFatal(boolean transitioning, boolean closed) {
    return !transitioning && !closed;
  }

  /** Maps device-global Camera2 frame numbers to the encoder's zero-based session ordinal. */
  public static long highSpeedOrdinal(long firstHighSpeedFrameNumber, long frameNumber) {
    if (firstHighSpeedFrameNumber < 0 || frameNumber < firstHighSpeedFrameNumber) {
      throw new IllegalArgumentException("high-speed Camera2 frame numbers are invalid");
    }
    return Math.subtractExact(frameNumber, firstHighSpeedFrameNumber);
  }

  /**
   * Estimates BOOTTIME minus MONOTONIC using one BOOTTIME reading bracketed by MONOTONIC reads.
   */
  public static long midpointClockOffsetNanos(
      long monotonicBeforeNanos, long boottimeNanos, long monotonicAfterNanos) {
    if (monotonicBeforeNanos < 0
        || boottimeNanos < 0
        || monotonicAfterNanos < monotonicBeforeNanos) {
      throw new IllegalArgumentException("clock anchor samples are invalid");
    }
    long midpoint =
        monotonicBeforeNanos
            + Math.floorDiv(
                Math.subtractExact(monotonicAfterNanos, monotonicBeforeNanos), 2L);
    return Math.subtractExact(boottimeNanos, midpoint);
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
