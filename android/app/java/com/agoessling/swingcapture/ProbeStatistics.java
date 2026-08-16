package com.agoessling.swingcapture;

import java.util.HashSet;
import java.util.Set;

/** Incremental timing statistics shared by host tests and Android capture probes. */
public final class ProbeStatistics {
  private long count;
  private final Set<Long> distinctValues = new HashSet<>();
  private long first;
  private long last;
  private long minimumPositiveDelta = Long.MAX_VALUE;
  private long maximumPositiveDelta;
  private long duplicateCount;
  private long nonmonotonicCount;

  public synchronized void add(long timestamp) {
    if (count == 0) {
      first = timestamp;
      last = timestamp;
      count = 1;
      distinctValues.add(timestamp);
      return;
    }

    long delta = timestamp - last;
    if (delta == 0) {
      ++duplicateCount;
    } else {
      if (delta < 0) {
        ++nonmonotonicCount;
      } else {
        minimumPositiveDelta = Math.min(minimumPositiveDelta, delta);
        maximumPositiveDelta = Math.max(maximumPositiveDelta, delta);
      }
    }
    distinctValues.add(timestamp);
    last = timestamp;
    ++count;
  }

  public synchronized long count() {
    return count;
  }

  public synchronized long distinctCount() {
    return distinctValues.size();
  }

  public synchronized long first() {
    return count == 0 ? 0 : first;
  }

  public synchronized long last() {
    return count == 0 ? 0 : last;
  }

  public synchronized long span() {
    return count < 2 ? 0 : last - first;
  }

  public synchronized long minimumPositiveDelta() {
    return minimumPositiveDelta == Long.MAX_VALUE ? 0 : minimumPositiveDelta;
  }

  public synchronized long maximumPositiveDelta() {
    return maximumPositiveDelta;
  }

  public synchronized long duplicateCount() {
    return duplicateCount;
  }

  public synchronized long nonmonotonicCount() {
    return nonmonotonicCount;
  }

  public synchronized double measuredRate(double timestampUnitsPerSecond) {
    long span = span();
    if (span <= 0 || distinctValues.size() < 2 || nonmonotonicCount != 0) {
      return 0.0;
    }
    return (distinctValues.size() - 1) * timestampUnitsPerSecond / span;
  }
}
