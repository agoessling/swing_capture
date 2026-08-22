package com.agoessling.swingcapture;

public final class PeerArmStatusTrackerTest {
  private PeerArmStatusTrackerTest() {}

  public static void main(String[] arguments) {
    PeerArmStatusTracker tracker = new PeerArmStatusTracker();
    tracker.pending("pose-one");
    check(tracker.snapshot().state() == PeerArmStatusTracker.State.PENDING, "pending");
    tracker.response("pose-one", false, 409);
    check(tracker.snapshot().state() == PeerArmStatusTracker.State.REJECTED, "rejected");
    check(tracker.snapshot().httpStatus() == 409, "rejected status");

    tracker.pending("pose-two");
    tracker.response("pose-one", true, 201);
    check(tracker.snapshot().state() == PeerArmStatusTracker.State.PENDING, "stale ignored");
    tracker.failed("pose-two", new java.io.IOException("offline"));
    check(tracker.snapshot().state() == PeerArmStatusTracker.State.FAILED, "failed");
    check(tracker.snapshot().failureType().equals("java.io.IOException"), "bounded type");

    tracker.inboundAccepted("pose-shadow");
    check(
        tracker.snapshot().state() == PeerArmStatusTracker.State.INBOUND_ACCEPTED,
        "inbound accepted");
    tracker.reset();
    check(tracker.snapshot().state() == PeerArmStatusTracker.State.NOT_REQUESTED, "reset");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
