package com.agoessling.swingcapture;

import java.util.Objects;
import java.util.function.Consumer;

/** Prevents one unexpected poll failure from cancelling every later scheduled health sample. */
final class PairNetworkHealthPollTask implements Runnable {
  private final Runnable poll;
  private final Consumer<RuntimeException> failureHandler;

  PairNetworkHealthPollTask(Runnable poll, Consumer<RuntimeException> failureHandler) {
    this.poll = Objects.requireNonNull(poll, "poll");
    this.failureHandler = Objects.requireNonNull(failureHandler, "failureHandler");
  }

  @Override
  public void run() {
    try {
      poll.run();
    } catch (RuntimeException failure) {
      try {
        failureHandler.accept(failure);
      } catch (RuntimeException ignored) {
        // Even diagnostic handling must not escape and permanently cancel fixed-delay execution.
      }
    }
  }
}
