package com.agoessling.swingcapture;

import java.util.concurrent.TimeUnit;

/** Lifecycle barrier for the debug JPEG associated with the service-accepted pose arm decision. */
final class ArmEvidenceFlush {
  enum Outcome {
    NOT_REQUESTED,
    PRESENT,
    FAILED,
    TIMED_OUT
  }

  enum TimeoutPolicy {
    PROCEED_WITHOUT_ARM_JPEG
  }

  private long requestedTimestampNs = -1;
  private boolean accepted;
  private Outcome outcome = Outcome.NOT_REQUESTED;

  synchronized void request(long timestampNs) {
    if (timestampNs < 0) {
      throw new IllegalArgumentException("arm evidence timestamp cannot be negative");
    }
    if (requestedTimestampNs >= 0 && requestedTimestampNs != timestampNs) {
      throw new IllegalStateException("arm evidence flush is already assigned");
    }
    requestedTimestampNs = timestampNs;
  }

  synchronized void accept(long timestampNs) {
    if (timestampNs != requestedTimestampNs) {
      throw new IllegalStateException("accepted arm evidence does not match its candidate");
    }
    accepted = true;
    notifyAll();
  }

  synchronized void cancel(long timestampNs) {
    if (timestampNs != requestedTimestampNs || accepted) {
      return;
    }
    requestedTimestampNs = -1;
    outcome = Outcome.NOT_REQUESTED;
    notifyAll();
  }

  synchronized void complete(long timestampNs, boolean present) {
    if (timestampNs != requestedTimestampNs || outcome != Outcome.NOT_REQUESTED) {
      return;
    }
    outcome = present ? Outcome.PRESENT : Outcome.FAILED;
    notifyAll();
  }

  synchronized Outcome await(long timeout, TimeUnit unit) throws InterruptedException {
    if (timeout < 0) {
      throw new IllegalArgumentException("arm evidence flush timeout cannot be negative");
    }
    if (unit == null) {
      throw new NullPointerException("unit");
    }
    if (!accepted || outcome != Outcome.NOT_REQUESTED) {
      return outcome;
    }
    long timeoutNs = unit.toNanos(timeout);
    long deadlineNs = saturatedAdd(System.nanoTime(), timeoutNs);
    while (outcome == Outcome.NOT_REQUESTED) {
      long remainingNs = deadlineNs - System.nanoTime();
      if (remainingNs <= 0) {
        outcome = Outcome.TIMED_OUT;
        return outcome;
      }
      long millis = remainingNs / 1_000_000L;
      int nanos = (int) (remainingNs % 1_000_000L);
      wait(millis, nanos);
    }
    return outcome;
  }

  synchronized boolean accepted() {
    return accepted;
  }

  synchronized boolean permitsOrdinaryWork() {
    return requestedTimestampNs < 0;
  }

  static TimeoutPolicy timeoutPolicy() {
    // Debug evidence must not turn a valid address detection into a missed backswing. The transfer
    // continues at its existing total deadline, retaining the synchronous observation row and an
    // explicit timeout metric while the best-effort JPEG worker is cancelled.
    return TimeoutPolicy.PROCEED_WITHOUT_ARM_JPEG;
  }

  private static long saturatedAdd(long left, long right) {
    if (right > 0 && left > Long.MAX_VALUE - right) {
      return Long.MAX_VALUE;
    }
    return left + right;
  }
}
