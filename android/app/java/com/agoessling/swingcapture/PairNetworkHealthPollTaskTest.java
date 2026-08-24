package com.agoessling.swingcapture;

import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

public final class PairNetworkHealthPollTaskTest {
  private PairNetworkHealthPollTaskTest() {}

  public static void main(String[] arguments) {
    continuesAfterPollFailure();
    containsFailureHandlerException();
  }

  private static void continuesAfterPollFailure() {
    AtomicInteger attempts = new AtomicInteger();
    AtomicReference<RuntimeException> retained = new AtomicReference<>();
    RuntimeException failure = new IllegalStateException("bad configuration");
    PairNetworkHealthPollTask task =
        new PairNetworkHealthPollTask(
            () -> {
              if (attempts.getAndIncrement() == 0) {
                throw failure;
              }
            },
            retained::set);

    task.run();
    task.run();
    check(attempts.get() == 2, "a later scheduled invocation still runs");
    check(retained.get() == failure, "the original failure reaches diagnostics");
  }

  private static void containsFailureHandlerException() {
    PairNetworkHealthPollTask task =
        new PairNetworkHealthPollTask(
            () -> {
              throw new IllegalStateException("poll");
            },
            failure -> {
              throw new IllegalStateException("diagnostics");
            });
    task.run();
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
