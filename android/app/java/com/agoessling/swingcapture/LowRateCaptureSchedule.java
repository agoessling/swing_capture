package com.agoessling.swingcapture;

/** Fixed-period single-shot scheduling without adding camera callback latency to every period. */
public final class LowRateCaptureSchedule {
  private static final long NANOS_PER_MILLISECOND = 1_000_000L;

  private LowRateCaptureSchedule() {}

  public static long nextDelayMillis(
      long requestSubmittedNs, long captureCompletedNs, long targetIntervalMillis) {
    if (requestSubmittedNs < 0 || captureCompletedNs < requestSubmittedNs) {
      throw new IllegalArgumentException("capture completion cannot precede request submission");
    }
    if (targetIntervalMillis <= 0) {
      throw new IllegalArgumentException("target interval must be positive");
    }
    long targetIntervalNs = Math.multiplyExact(targetIntervalMillis, NANOS_PER_MILLISECOND);
    long elapsedNs = Math.subtractExact(captureCompletedNs, requestSubmittedNs);
    long remainingNs = Math.max(0, Math.subtractExact(targetIntervalNs, elapsedNs));
    return Math.floorDiv(Math.addExact(remainingNs, NANOS_PER_MILLISECOND - 1),
        NANOS_PER_MILLISECOND);
  }
}
