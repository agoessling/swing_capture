package com.agoessling.swingcapture;

import java.util.Objects;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import java.util.function.LongSupplier;
import java.util.function.Supplier;

/** Bounded monitor wait that returns as soon as a caller-supplied candidate becomes available. */
final class MonitorCandidateWait {
  private MonitorCandidateWait() {}

  static <T> T until(
      Object monitor,
      LongSupplier nowNanos,
      long deadlineNanos,
      BooleanSupplier cancelled,
      Supplier<T> candidate) {
    Objects.requireNonNull(monitor, "monitor");
    Objects.requireNonNull(nowNanos, "nowNanos");
    Objects.requireNonNull(cancelled, "cancelled");
    Objects.requireNonNull(candidate, "candidate");
    synchronized (monitor) {
      while (true) {
        T available = candidate.get();
        if (available != null || cancelled.getAsBoolean()) {
          return available;
        }
        long remainingNanos = deadlineNanos - nowNanos.getAsLong();
        if (remainingNanos <= 0) {
          return candidate.get();
        }
        try {
          TimeUnit.NANOSECONDS.timedWait(monitor, remainingNanos);
        } catch (InterruptedException interrupted) {
          Thread.currentThread().interrupt();
          return candidate.get();
        }
      }
    }
  }
}
