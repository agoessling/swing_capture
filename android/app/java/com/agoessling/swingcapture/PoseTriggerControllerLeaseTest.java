package com.agoessling.swingcapture;

import com.agoessling.swingcapture.pose.PoseTriggerController;

/** Regression coverage for finish-pose suppression across a high-speed capture cycle. */
public final class PoseTriggerControllerLeaseTest {
  private static final long MS = 1_000_000L;

  private PoseTriggerControllerLeaseTest() {}

  public static void main(String[] arguments) {
    preservesWaitingForClearAcrossEngineRestart();
    releaseStartsAFreshArmCycle();
    rejectsConfigurationChangeWithinArmCycle();
    externalCaptureCanEndFromShadowWatchingState();
    timestampIsSelectedInsideControllerCriticalSection();
  }

  private static void preservesWaitingForClearAcrossEngineRestart() {
    PoseTriggerController.Config config = PoseTriggerController.Config.defaultsForFiveFramesPerSecond();
    PoseTriggerControllerLease lease = new PoseTriggerControllerLease();
    PoseTriggerController first = lease.acquire(config);
    first.observe(address(0));
    first.observe(address(200 * MS));
    PoseTriggerController.Decision armed = first.observe(address(400 * MS));
    check(armed.command() == PoseTriggerController.Command.START_HIGH_SPEED, "controller armed");

    PoseTriggerController.Decision ended = lease.captureEnded(3_000 * MS);
    check(ended.state() == PoseTriggerController.State.WAITING_FOR_CLEAR, "capture ended state");
    PoseTriggerController restarted = lease.acquire(config);
    check(restarted == first, "same controller survives engine restart");
    PoseTriggerController.Decision finish = restarted.observe(address(3_200 * MS));
    check(finish.command() == PoseTriggerController.Command.NONE, "finish pose cannot rearm");
    check(
        finish.state() == PoseTriggerController.State.WAITING_FOR_CLEAR,
        "finish pose remains suppressed");
  }

  private static void releaseStartsAFreshArmCycle() {
    PoseTriggerControllerLease lease = new PoseTriggerControllerLease();
    PoseTriggerController.Config config = PoseTriggerController.Config.defaultsForFiveFramesPerSecond();
    PoseTriggerController previous = lease.acquire(config);
    lease.release();
    PoseTriggerController fresh = lease.acquire(config);
    check(fresh != previous, "new physical arm cycle gets a new controller");
    check(fresh.state() == PoseTriggerController.State.WATCHING, "fresh controller state");
  }

  private static void rejectsConfigurationChangeWithinArmCycle() {
    PoseTriggerControllerLease lease = new PoseTriggerControllerLease();
    PoseTriggerController.Config defaults = PoseTriggerController.Config.defaultsForFiveFramesPerSecond();
    lease.acquire(defaults);
    PoseTriggerController.Config changed =
        new PoseTriggerController.Config(
            defaults.minimumPersonConfidence(),
            defaults.maximumClearPersonConfidence(),
            defaults.minimumAddressConfidence(),
            defaults.maximumMotionMagnitude(),
            defaults.minimumQualificationNs() + MS,
            defaults.maximumObservationGapNs(),
            defaults.qualificationDropoutGraceNs(),
            defaults.maximumArmedDurationNs(),
            defaults.clearDurationNs(),
            defaults.cooldownNs(),
            defaults.thermalHardCapNs());
    expectFailure(() -> lease.acquire(changed));
  }

  private static void externalCaptureCanEndFromShadowWatchingState() {
    PoseTriggerControllerLease lease = new PoseTriggerControllerLease();
    lease.acquire(PoseTriggerController.Config.defaultsForFiveFramesPerSecond());
    PoseTriggerController.Decision started = lease.externalCaptureStarted(100 * MS);
    check(started.state() == PoseTriggerController.State.ARM_REQUESTED, "external lease started");
    PoseTriggerController.Decision ended = lease.captureEnded(200 * MS);
    check(ended.state() == PoseTriggerController.State.WAITING_FOR_CLEAR, "external lease ended");
  }

  private static void timestampIsSelectedInsideControllerCriticalSection() {
    PoseTriggerControllerLease lease = new PoseTriggerControllerLease();
    PoseTriggerController controller =
        lease.acquire(PoseTriggerController.Config.defaultsForFiveFramesPerSecond());
    java.util.concurrent.CountDownLatch timestampRequested =
        new java.util.concurrent.CountDownLatch(1);
    java.util.concurrent.CountDownLatch allowTimestamp = new java.util.concurrent.CountDownLatch(1);
    Thread transition =
        new Thread(
            () ->
                lease.externalCaptureStarted(
                    () -> {
                      timestampRequested.countDown();
                      await(allowTimestamp);
                      return 100 * MS;
                    }));
    transition.start();
    await(timestampRequested);
    Thread inference =
        new Thread(
            () -> {
              synchronized (controller) {
                controller.observe(address(200 * MS));
              }
            });
    inference.start();
    allowTimestamp.countDown();
    join(transition);
    join(inference);
    check(
        controller.state() == PoseTriggerController.State.ARM_REQUESTED,
        "inference cannot advance timestamp between sampling and transition");
  }

  private static void await(java.util.concurrent.CountDownLatch latch) {
    try {
      latch.await();
    } catch (InterruptedException interrupted) {
      Thread.currentThread().interrupt();
      throw new AssertionError(interrupted);
    }
  }

  private static void join(Thread thread) {
    try {
      thread.join();
    } catch (InterruptedException interrupted) {
      Thread.currentThread().interrupt();
      throw new AssertionError(interrupted);
    }
  }

  private static PoseTriggerController.Observation address(long timestampNs) {
    return new PoseTriggerController.Observation(timestampNs, 0.9, 0.9, 0.1, true);
  }

  private static void expectFailure(Runnable action) {
    try {
      action.run();
      throw new AssertionError("configuration change did not fail");
    } catch (IllegalStateException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
