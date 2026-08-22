package com.agoessling.swingcapture;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicLong;

/** Cross-instance optimistic-update regression for the process-wide setup lock. */
public final class NodeConfigurationProcessLockTest {
  private NodeConfigurationProcessLockTest() {}

  public static void main(String[] arguments) throws Exception {
    AtomicLong sharedRevision = new AtomicLong(7);
    CountDownLatch ready = new CountDownLatch(2);
    CountDownLatch start = new CountDownLatch(1);
    List<String> outcomes = java.util.Collections.synchronizedList(new ArrayList<>());
    Runnable independentConfigurationInstance =
        () -> {
          ready.countDown();
          await(start);
          try {
            NodeConfigurationProcessLock.run(
                () -> {
                  if (sharedRevision.get() != 7) {
                    throw new StaleUpdate();
                  }
                  sharedRevision.set(8);
                });
            outcomes.add("committed");
          } catch (StaleUpdate stale) {
            outcomes.add("conflict");
          }
        };
    Thread activity = new Thread(independentConfigurationInstance, "activity-config");
    Thread service = new Thread(independentConfigurationInstance, "service-config");
    activity.start();
    service.start();
    ready.await();
    start.countDown();
    activity.join();
    service.join();

    check(sharedRevision.get() == 8, "exactly one generation committed");
    check(outcomes.stream().filter("committed"::equals).count() == 1, "one commit");
    check(outcomes.stream().filter("conflict"::equals).count() == 1, "one conflict");
  }

  private static void await(CountDownLatch latch) {
    try {
      latch.await();
    } catch (InterruptedException interrupted) {
      Thread.currentThread().interrupt();
      throw new AssertionError("interrupted", interrupted);
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }

  private static final class StaleUpdate extends RuntimeException {}
}
