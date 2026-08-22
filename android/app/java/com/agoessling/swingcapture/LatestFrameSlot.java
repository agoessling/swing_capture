package com.agoessling.swingcapture;

import java.util.Objects;
import java.util.concurrent.TimeUnit;

/**
 * Thread-safe, latest-value-wins handoff with capacity for one running and one pending value.
 *
 * <p>The slot deliberately does not dispose values. Every value returned as dropped or rejected
 * remains owned by the caller, which makes resource ownership explicit for Camera2 Images.
 */
final class LatestFrameSlot<T> {
  record Offer<T>(boolean accepted, T scheduleNow, T dropped) {}

  private boolean accepting = true;
  private boolean running;
  private T pending;

  synchronized Offer<T> offer(T value) {
    Objects.requireNonNull(value, "value");
    if (!accepting) {
      return new Offer<>(false, null, value);
    }
    if (!running) {
      running = true;
      return new Offer<>(true, value, null);
    }
    T dropped = pending;
    pending = value;
    return new Offer<>(true, null, dropped);
  }

  /** Completes the current value and transfers ownership of the next pending value, if any. */
  synchronized T completeAndTakeNext() {
    if (!running) {
      throw new IllegalStateException("no current value is running");
    }
    if (pending != null && accepting) {
      T next = pending;
      pending = null;
      return next;
    }
    pending = null;
    running = false;
    notifyAll();
    return null;
  }

  /** Stops future work and transfers ownership of the pending value, if any, to the caller. */
  synchronized T stopAcceptingAndTakePending() {
    accepting = false;
    T detached = pending;
    pending = null;
    if (!running) {
      notifyAll();
    }
    return detached;
  }

  synchronized boolean awaitIdle(long timeout, TimeUnit unit) throws InterruptedException {
    if (timeout < 0) {
      throw new IllegalArgumentException("timeout cannot be negative");
    }
    Objects.requireNonNull(unit, "unit");
    long timeoutNs = unit.toNanos(timeout);
    long deadlineNs = saturatedAdd(System.nanoTime(), timeoutNs);
    while (running) {
      long remainingNs = deadlineNs - System.nanoTime();
      if (remainingNs <= 0) {
        return false;
      }
      long millis = remainingNs / 1_000_000L;
      int nanos = (int) (remainingNs % 1_000_000L);
      wait(millis, nanos);
    }
    return true;
  }

  synchronized int retainedValueCount() {
    return (running ? 1 : 0) + (pending == null ? 0 : 1);
  }

  synchronized boolean accepting() {
    return accepting;
  }

  private static long saturatedAdd(long left, long right) {
    if (right > 0 && left > Long.MAX_VALUE - right) {
      return Long.MAX_VALUE;
    }
    return left + right;
  }
}
