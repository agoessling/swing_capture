package com.agoessling.swingcapture;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

public final class MonitorCandidateWaitTest {
  private MonitorCandidateWaitTest() {}

  public static void main(String[] args) throws InterruptedException {
    returnsAnAlreadyAvailableCandidateBeforeReadingTheClock();
    returnsWhenANotifiedCandidateArrives();
    checksOneFinalCandidateAtTheDeadline();
  }

  private static void returnsAnAlreadyAvailableCandidateBeforeReadingTheClock() {
    AtomicInteger clockReads = new AtomicInteger();
    String result =
        MonitorCandidateWait.until(
            new Object(),
            () -> {
              clockReads.incrementAndGet();
              return 0;
            },
            100,
            () -> false,
            () -> "ready");
    check(result.equals("ready"), "preexisting candidate");
    check(clockReads.get() == 0, "preexisting candidate must not consume the wait budget");
  }

  private static void returnsWhenANotifiedCandidateArrives() throws InterruptedException {
    Object monitor = new Object();
    CountDownLatch firstProbe = new CountDownLatch(1);
    AtomicReference<String> candidate = new AtomicReference<>();
    AtomicReference<String> result = new AtomicReference<>();
    long deadlineNanos = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
    Thread worker =
        new Thread(
            () ->
                result.set(
                    MonitorCandidateWait.until(
                        monitor,
                        System::nanoTime,
                        deadlineNanos,
                        () -> false,
                        () -> {
                          firstProbe.countDown();
                          return candidate.get();
                        })));
    worker.setDaemon(true);
    worker.start();
    check(firstProbe.await(1, TimeUnit.SECONDS), "waiter made its initial probe");
    synchronized (monitor) {
      candidate.set("notified");
      monitor.notifyAll();
    }
    worker.join(1_000);
    check(!worker.isAlive(), "waiter returned before its deadline");
    check("notified".equals(result.get()), "notified candidate");
  }

  private static void checksOneFinalCandidateAtTheDeadline() {
    AtomicInteger probes = new AtomicInteger();
    String result =
        MonitorCandidateWait.until(
            new Object(),
            () -> 100,
            100,
            () -> false,
            () -> probes.incrementAndGet() == 2 ? "boundary" : null);
    check(result.equals("boundary"), "deadline candidate");
    check(probes.get() == 2, "candidate is checked before and at the deadline");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
