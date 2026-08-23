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
    check(
        tracker.classifyInboundRequest("pose-shadow")
            == PeerArmStatusTracker.InboundRequestDisposition.DUPLICATE_ACTIVE,
        "active inbound retry is idempotent");
    tracker.reset();
    check(tracker.snapshot().state() == PeerArmStatusTracker.State.NOT_REQUESTED, "reset");
    check(
        tracker.classifyInboundRequest("pose-shadow")
            == PeerArmStatusTracker.InboundRequestDisposition.STALE,
        "completed inbound session cannot re-arm after monitoring restarts");
    check(
        tracker.classifyInboundRequest("pose-new")
            == PeerArmStatusTracker.InboundRequestDisposition.NEW,
        "fresh inbound session remains admissible");

    tracker.inboundAccepted("pose-partial");
    tracker.inboundFailed("pose-partial", new IllegalStateException("camera transfer failed"));
    check(tracker.snapshot().state() == PeerArmStatusTracker.State.FAILED, "partial arm failed");
    check(
        tracker.classifyInboundRequest("pose-partial")
            == PeerArmStatusTracker.InboundRequestDisposition.STALE,
        "abandoned partial arm cannot be replayed");

    tracker.pending("pose-current");
    tracker.response("pose-old", true, 202);
    tracker.failed("pose-old", new java.io.IOException("late failure"));
    check(
        tracker.snapshot().state() == PeerArmStatusTracker.State.PENDING,
        "reordered outcome from an old session is ignored");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
