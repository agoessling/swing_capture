package com.agoessling.swingcapture;

import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class ArmEvidenceFlushTest {
  private record Work(long timestampNs, boolean arm) {}

  public static void main(String[] args) throws Exception {
    armWorkReplacesOrdinaryPendingAndIsPresentBeforeTransferSnapshot();
    canceledArmResumesOrdinaryEvidenceAndSecondArmFlushesCorrectFrame();
    timeoutProceedsWithoutBlockingHighSpeedIndefinitely();
  }

  private static void armWorkReplacesOrdinaryPendingAndIsPresentBeforeTransferSnapshot()
      throws Exception {
    LatestFrameSlot<Work> slot = new LatestFrameSlot<>();
    ArmEvidenceFlush flush = new ArmEvidenceFlush();
    Set<Long> attached = new HashSet<>();
    CountDownLatch ordinaryStarted = new CountDownLatch(1);
    CountDownLatch releaseOrdinary = new CountDownLatch(1);

    Work ordinary = new Work(1, false);
    Work first = slot.offer(ordinary).scheduleNow();
    Thread worker =
        new Thread(
            () -> {
              Work current = first;
              while (current != null) {
                try {
                  if (!current.arm()) {
                    ordinaryStarted.countDown();
                    releaseOrdinary.await();
                  }
                  attached.add(current.timestampNs());
                  if (current.arm()) {
                    flush.complete(current.timestampNs(), true);
                  }
                } catch (InterruptedException interrupted) {
                  Thread.currentThread().interrupt();
                  return;
                }
                current = slot.completeAndTakeNext();
              }
            },
            "arm-evidence-flush-test");
    worker.setDaemon(true);
    worker.start();
    check(ordinaryStarted.await(1, TimeUnit.SECONDS), "ordinary evidence worker did not start");
    slot.offer(new Work(2, false));
    Work arm = new Work(3, true);
    flush.request(arm.timestampNs());
    check(!flush.permitsOrdinaryWork(), "ordinary work must not supersede a pending arm JPEG");
    flush.accept(arm.timestampNs());
    LatestFrameSlot.Offer<Work> armOffer = slot.offer(arm);
    check(armOffer.dropped().timestampNs() == 2, "arm work must replace ordinary pending work");
    releaseOrdinary.countDown();

    check(flush.await(1, TimeUnit.SECONDS) == ArmEvidenceFlush.Outcome.PRESENT, "arm flush");
    check(attached.contains(arm.timestampNs()), "snapshot boundary must observe the arm JPEG");
    check(slot.awaitIdle(1, TimeUnit.SECONDS), "evidence slot should become idle");
    worker.join(1_000);
    check(!worker.isAlive(), "evidence worker should terminate");
  }

  private static void canceledArmResumesOrdinaryEvidenceAndSecondArmFlushesCorrectFrame()
      throws Exception {
    LatestFrameSlot<Work> slot = new LatestFrameSlot<>();
    ArmEvidenceFlush flush = new ArmEvidenceFlush();
    Set<Long> attached = new HashSet<>();
    CountDownLatch workerStarted = new CountDownLatch(1);
    CountDownLatch releaseWorker = new CountDownLatch(1);
    Work first = slot.offer(new Work(1, false)).scheduleNow();
    Thread worker =
        new Thread(
            () -> {
              Work current = first;
              while (current != null) {
                try {
                  if (current.timestampNs() == 1) {
                    workerStarted.countDown();
                    releaseWorker.await();
                  }
                  attached.add(current.timestampNs());
                  if (current.arm()) {
                    flush.complete(current.timestampNs(), true);
                  }
                } catch (InterruptedException interrupted) {
                  Thread.currentThread().interrupt();
                  return;
                }
                current = slot.completeAndTakeNext();
              }
            },
            "arm-evidence-rearm-test");
    worker.setDaemon(true);
    worker.start();
    check(workerStarted.await(1, TimeUnit.SECONDS), "rearm worker did not start");

    Work canceledArm = new Work(10, true);
    flush.request(canceledArm.timestampNs());
    slot.offer(canceledArm);
    flush.cancel(canceledArm.timestampNs());
    check(flush.permitsOrdinaryWork(), "diagnostic veto must resume ordinary evidence");
    Work ordinary = new Work(11, false);
    LatestFrameSlot.Offer<Work> ordinaryOffer = slot.offer(ordinary);
    check(
        ordinaryOffer.dropped().timestampNs() == canceledArm.timestampNs(),
        "ordinary evidence should replace the canceled arm frame");
    flush.complete(canceledArm.timestampNs(), false);

    Work acceptedArm = new Work(20, true);
    flush.request(acceptedArm.timestampNs());
    LatestFrameSlot.Offer<Work> acceptedOffer = slot.offer(acceptedArm);
    check(
        acceptedOffer.dropped().timestampNs() == ordinary.timestampNs(),
        "second arm should take priority over ordinary pending evidence");
    flush.accept(acceptedArm.timestampNs());
    releaseWorker.countDown();

    check(
        flush.await(1, TimeUnit.SECONDS) == ArmEvidenceFlush.Outcome.PRESENT,
        "accepted second arm should flush");
    check(attached.contains(acceptedArm.timestampNs()), "second arm JPEG must be attached");
    check(!attached.contains(canceledArm.timestampNs()), "canceled arm JPEG must not be selected");
    check(slot.awaitIdle(1, TimeUnit.SECONDS), "rearm evidence slot should become idle");
    worker.join(1_000);
    check(!worker.isAlive(), "rearm evidence worker should terminate");
  }

  private static void timeoutProceedsWithoutBlockingHighSpeedIndefinitely() throws Exception {
    ArmEvidenceFlush flush = new ArmEvidenceFlush();
    check(flush.permitsOrdinaryWork(), "ordinary evidence is allowed before an arm decision");
    flush.request(7);
    flush.accept(7);
    check(
        flush.await(0, TimeUnit.NANOSECONDS) == ArmEvidenceFlush.Outcome.TIMED_OUT,
        "bounded flush should time out");
    check(
        ArmEvidenceFlush.timeoutPolicy()
            == ArmEvidenceFlush.TimeoutPolicy.PROCEED_WITHOUT_ARM_JPEG,
        "timeout policy must preserve high-speed transition");
    flush.complete(7, true);
    check(
        flush.await(0, TimeUnit.MILLISECONDS) == ArmEvidenceFlush.Outcome.TIMED_OUT,
        "late completion cannot rewrite timeout evidence");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
