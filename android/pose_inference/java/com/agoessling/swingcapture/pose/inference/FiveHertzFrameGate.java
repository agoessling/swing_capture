package com.agoessling.swingcapture.pose.inference;

/** Selects at most five monotonically timestamped frames per second without wall-clock reads. */
public final class FiveHertzFrameGate {
  public static final long MIN_INTERVAL_NS = 200_000_000L;

  private long lastAcceptedTimestampNs = Long.MIN_VALUE;
  private long lastObservedTimestampNs = Long.MIN_VALUE;

  public boolean accept(long timestampNs) {
    if (timestampNs < 0) {
      throw new IllegalArgumentException("timestampNs cannot be negative");
    }
    if (lastObservedTimestampNs != Long.MIN_VALUE && timestampNs < lastObservedTimestampNs) {
      throw new IllegalArgumentException("frame timestamps must be monotonic");
    }
    lastObservedTimestampNs = timestampNs;
    if (lastAcceptedTimestampNs == Long.MIN_VALUE) {
      lastAcceptedTimestampNs = timestampNs;
      return true;
    }
    if (timestampNs - lastAcceptedTimestampNs < MIN_INTERVAL_NS) {
      return false;
    }
    lastAcceptedTimestampNs = timestampNs;
    return true;
  }
}
