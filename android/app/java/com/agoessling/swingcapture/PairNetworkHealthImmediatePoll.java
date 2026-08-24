package com.agoessling.swingcapture;

import java.util.Objects;
import java.util.concurrent.Executor;

/**
 * Coalesces configuration-triggered samples while guaranteeing that a changed target is re-polled.
 */
final class PairNetworkHealthImmediatePoll {
  private final Executor executor;
  private final Runnable poll;
  private boolean scheduledOrRunning;
  private boolean rerunRequested;

  PairNetworkHealthImmediatePoll(Executor executor, Runnable poll) {
    this.executor = Objects.requireNonNull(executor, "executor");
    this.poll = Objects.requireNonNull(poll, "poll");
  }

  synchronized void request() {
    if (scheduledOrRunning) {
      rerunRequested = true;
      return;
    }
    scheduledOrRunning = true;
    try {
      executor.execute(this::runOnce);
    } catch (RuntimeException rejected) {
      scheduledOrRunning = false;
      throw rejected;
    }
  }

  private void runOnce() {
    try {
      poll.run();
    } finally {
      synchronized (this) {
        if (!rerunRequested) {
          scheduledOrRunning = false;
          return;
        }
        rerunRequested = false;
        try {
          executor.execute(this::runOnce);
        } catch (RuntimeException rejected) {
          scheduledOrRunning = false;
          throw rejected;
        }
      }
    }
  }
}
