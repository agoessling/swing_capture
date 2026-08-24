package com.agoessling.swingcapture;

import java.util.ArrayDeque;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicInteger;

public final class PairNetworkHealthImmediatePollTest {
  private PairNetworkHealthImmediatePollTest() {}

  public static void main(String[] arguments) {
    schedulesAnImmediatePoll();
    coalescesQueuedRequestsAndRetainsOneRerun();
    retainsARequestMadeWhilePolling();
    rejectedSubmissionCanBeRetried();
  }

  private static void schedulesAnImmediatePoll() {
    ManualExecutor executor = new ManualExecutor();
    AtomicInteger polls = new AtomicInteger();
    PairNetworkHealthImmediatePoll immediate =
        new PairNetworkHealthImmediatePoll(executor, polls::incrementAndGet);

    immediate.request();
    check(executor.size() == 1, "one immediate sample is queued");
    executor.runNext();
    check(polls.get() == 1, "queued sample executes");
  }

  private static void coalescesQueuedRequestsAndRetainsOneRerun() {
    ManualExecutor executor = new ManualExecutor();
    AtomicInteger polls = new AtomicInteger();
    PairNetworkHealthImmediatePoll immediate =
        new PairNetworkHealthImmediatePoll(executor, polls::incrementAndGet);

    immediate.request();
    immediate.request();
    immediate.request();
    check(executor.size() == 1, "queued requests do not overlap");
    executor.runNext();
    check(polls.get() == 1 && executor.size() == 1, "one changed-target rerun is retained");
    executor.runNext();
    check(polls.get() == 2 && executor.size() == 0, "coalesced rerun completes once");
  }

  private static void retainsARequestMadeWhilePolling() {
    ManualExecutor executor = new ManualExecutor();
    AtomicInteger polls = new AtomicInteger();
    PairNetworkHealthImmediatePoll[] holder = new PairNetworkHealthImmediatePoll[1];
    holder[0] =
        new PairNetworkHealthImmediatePoll(
            executor,
            () -> {
              if (polls.incrementAndGet() == 1) {
                holder[0].request();
              }
            });

    holder[0].request();
    executor.runNext();
    check(executor.size() == 1, "a target change during measurement queues a replacement");
    executor.runNext();
    check(polls.get() == 2, "replacement sample executes after the in-flight sample");
  }

  private static void rejectedSubmissionCanBeRetried() {
    AtomicInteger attempts = new AtomicInteger();
    Executor executor =
        runnable -> {
          if (attempts.incrementAndGet() == 1) {
            throw new IllegalStateException("rejected");
          }
          runnable.run();
        };
    AtomicInteger polls = new AtomicInteger();
    PairNetworkHealthImmediatePoll immediate =
        new PairNetworkHealthImmediatePoll(executor, polls::incrementAndGet);

    expectFailure(immediate::request);
    immediate.request();
    check(polls.get() == 1, "a rejected submission does not wedge future sampling");
  }

  private static void expectFailure(Runnable operation) {
    try {
      operation.run();
      throw new AssertionError("expected submission failure");
    } catch (IllegalStateException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }

  private static final class ManualExecutor implements Executor {
    private final ArrayDeque<Runnable> queued = new ArrayDeque<>();

    @Override
    public void execute(Runnable command) {
      queued.addLast(command);
    }

    int size() {
      return queued.size();
    }

    void runNext() {
      Runnable next = queued.pollFirst();
      if (next == null) {
        throw new AssertionError("no queued task");
      }
      next.run();
    }
  }
}
