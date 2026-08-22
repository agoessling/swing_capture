package com.agoessling.swingcapture;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;

/** Host coverage for lossless and coherent standby telemetry shared across service threads. */
public final class StandbyDiagnosticTelemetryTest {
  private static final int WORKER_COUNT = 8;
  private static final int ITERATIONS = 10_000;

  private StandbyDiagnosticTelemetryTest() {}

  public static void main(String[] arguments) throws Exception {
    concurrentCounterUpdatesAreLossless();
    droppedCountAndLastErrorArePublishedAtomically();
    timestampAndResetStateAreExplicit();
  }

  private static void concurrentCounterUpdatesAreLossless() throws Exception {
    StandbyDiagnosticTelemetry telemetry = new StandbyDiagnosticTelemetry();
    CountDownLatch start = new CountDownLatch(1);
    List<Thread> workers = new ArrayList<>();
    for (int index = 0; index < WORKER_COUNT; ++index) {
      workers.add(
          worker(
              start,
              () -> {
                telemetry.recordDetectedEvent();
                telemetry.recordOperatorTag();
                telemetry.recordPublishedSession();
                telemetry.addDroppedEvents(2);
                telemetry.recordDiscontinuity();
                telemetry.addEventsCancelledForPoseArm(3);
              }));
    }
    workers.forEach(Thread::start);
    start.countDown();
    for (Thread worker : workers) {
      worker.join();
      check(!worker.isAlive(), "counter worker completed");
    }

    StandbyDiagnosticTelemetry.Snapshot snapshot = telemetry.snapshot();
    long updates = WORKER_COUNT * (long) ITERATIONS;
    check(snapshot.detectedEvents() == updates, "detected events are lossless");
    check(snapshot.operatorTags() == updates, "operator tags are lossless");
    check(snapshot.publishedSessions() == updates, "published sessions are lossless");
    check(snapshot.droppedEvents() == updates * 2L, "dropped events are lossless");
    check(snapshot.discontinuities() == updates, "discontinuities are lossless");
    check(
        snapshot.eventsCancelledForPoseArm() == updates * 3L,
        "pose-arm cancellations are lossless");
  }

  private static void droppedCountAndLastErrorArePublishedAtomically() throws Exception {
    StandbyDiagnosticTelemetry telemetry = new StandbyDiagnosticTelemetry();
    CountDownLatch start = new CountDownLatch(1);
    List<Throwable> failures = new ArrayList<>();
    Thread writer =
        new Thread(
            () -> {
              await(start);
              for (int index = 1; index <= ITERATIONS; ++index) {
                telemetry.recordDroppedEvents(1, "publication:" + index);
              }
            });
    Thread reader =
        new Thread(
            () -> {
              await(start);
              try {
                while (writer.isAlive()) {
                  StandbyDiagnosticTelemetry.Snapshot snapshot = telemetry.snapshot();
                  if (snapshot.droppedEvents() > 0
                      && !snapshot.lastError().equals(
                          "publication:" + snapshot.droppedEvents())) {
                    throw new AssertionError("drop count and error were torn");
                  }
                }
              } catch (Throwable failure) {
                synchronized (failures) {
                  failures.add(failure);
                }
              }
            });
    writer.start();
    reader.start();
    start.countDown();
    writer.join();
    reader.join();
    check(failures.isEmpty(), "drop count and error snapshots remain coherent");
    StandbyDiagnosticTelemetry.Snapshot snapshot = telemetry.snapshot();
    check(snapshot.droppedEvents() == ITERATIONS, "all paired drops recorded");
    check(snapshot.lastError().equals("publication:" + ITERATIONS), "latest error retained");
  }

  private static void timestampAndResetStateAreExplicit() {
    StandbyDiagnosticTelemetry telemetry = new StandbyDiagnosticTelemetry();
    telemetry.recordTimestampRejections(100);
    telemetry.recordError("preview_snapshot:IOException");
    check(telemetry.snapshot().timestampRejections() == 100, "timestamp count recorded");
    telemetry.clearLastError();
    check(telemetry.snapshot().lastError().isEmpty(), "last error cleared without losing counters");
  }

  private static Thread worker(CountDownLatch start, Runnable update) {
    return new Thread(
        () -> {
          await(start);
          for (int index = 0; index < ITERATIONS; ++index) {
            update.run();
          }
        });
  }

  private static void await(CountDownLatch latch) {
    try {
      latch.await();
    } catch (InterruptedException interruption) {
      Thread.currentThread().interrupt();
      throw new AssertionError("telemetry test interrupted", interruption);
    }
  }

  private static void check(boolean condition, String label) {
    if (!condition) {
      throw new AssertionError(label);
    }
  }
}
