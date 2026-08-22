package com.agoessling.swingcapture;

import java.util.concurrent.TimeUnit;

public final class LatestFrameSlotTest {
  public static void main(String[] args) throws Exception {
    schedulesFirstAndRetainsOnlyLatestPending();
    stopRejectsNewWorkAndDetachesPending();
    idleWaitHasDeterministicCompletionAndTimeout();
    terminalWorkerFailureReleasesCurrentAndPendingOwnership();
  }

  private static void schedulesFirstAndRetainsOnlyLatestPending() {
    LatestFrameSlot<String> slot = new LatestFrameSlot<>();
    LatestFrameSlot.Offer<String> first = slot.offer("first");
    check(first.accepted(), "first should be accepted");
    check("first".equals(first.scheduleNow()), "first should schedule immediately");
    check(first.dropped() == null, "first should not drop anything");

    LatestFrameSlot.Offer<String> second = slot.offer("second");
    check(second.accepted() && second.scheduleNow() == null, "second should be pending");
    check(second.dropped() == null, "second should not replace anything");
    check(slot.retainedValueCount() == 2, "slot must retain current plus one pending");

    LatestFrameSlot.Offer<String> third = slot.offer("third");
    check("second".equals(third.dropped()), "latest offer should replace the old pending value");
    check(slot.retainedValueCount() == 2, "replacement must not increase capacity");
    check("third".equals(slot.completeAndTakeNext()), "completion should select latest pending");
    check(slot.retainedValueCount() == 1, "only the transferred current value should remain");
    check(slot.completeAndTakeNext() == null, "final completion should become idle");
    check(slot.retainedValueCount() == 0, "idle slot should retain no values");
  }

  private static void stopRejectsNewWorkAndDetachesPending() {
    LatestFrameSlot<String> slot = new LatestFrameSlot<>();
    slot.offer("current");
    slot.offer("pending");
    check("pending".equals(slot.stopAcceptingAndTakePending()), "stop should detach pending work");
    check(!slot.accepting(), "slot should stop accepting");
    LatestFrameSlot.Offer<String> rejected = slot.offer("late");
    check(!rejected.accepted(), "late work should be rejected");
    check("late".equals(rejected.dropped()), "rejected value must be returned for disposal");
    check(slot.completeAndTakeNext() == null, "current completion should become idle after stop");
  }

  private static void idleWaitHasDeterministicCompletionAndTimeout() throws Exception {
    LatestFrameSlot<String> idle = new LatestFrameSlot<>();
    check(idle.awaitIdle(0, TimeUnit.MILLISECONDS), "an idle slot should not consume timeout");

    LatestFrameSlot<String> running = new LatestFrameSlot<>();
    running.offer("current");
    check(!running.awaitIdle(1, TimeUnit.MILLISECONDS), "running slot should time out");
    running.completeAndTakeNext();
    check(running.awaitIdle(0, TimeUnit.MILLISECONDS), "completed slot should be idle");
  }

  private static void terminalWorkerFailureReleasesCurrentAndPendingOwnership() throws Exception {
    LatestFrameSlot<String> slot = new LatestFrameSlot<>();
    check("current".equals(slot.offer("current").scheduleNow()), "terminal current");
    slot.offer("pending");
    check(
        "pending".equals(slot.stopAcceptingAndTakePending()),
        "terminal path detaches pending ownership");
    check(slot.completeAndTakeNext() == null, "terminal path completes current ownership");
    check(slot.awaitIdle(0, TimeUnit.MILLISECONDS), "terminal worker cannot leave slot running");
    check(!slot.offer("late").accepted(), "terminal worker permanently rejects late images");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
