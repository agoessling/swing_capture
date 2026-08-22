package com.agoessling.swingcapture;

/** Reduces optional preview encoding load while always retaining a pose-arm decision frame. */
final class DebugEvidenceCadence {
  static final long DEFAULT_INTERVAL_NANOS = 1_000_000_000L;

  private final long intervalNanos;
  private long lastAcceptedTimestampNanos = -1;

  DebugEvidenceCadence(long intervalNanos) {
    if (intervalNanos <= 0) {
      throw new IllegalArgumentException("debug evidence interval must be positive");
    }
    this.intervalNanos = intervalNanos;
  }

  boolean shouldSnapshot(long timestampNanos, boolean armDecision) {
    if (timestampNanos < 0) {
      throw new IllegalArgumentException("debug evidence timestamp cannot be negative");
    }
    if (lastAcceptedTimestampNanos < 0
        || armDecision
        || timestampNanos - lastAcceptedTimestampNanos >= intervalNanos) {
      lastAcceptedTimestampNanos = timestampNanos;
      return true;
    }
    return false;
  }
}
