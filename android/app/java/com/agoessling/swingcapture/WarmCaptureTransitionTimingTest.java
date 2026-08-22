package com.agoessling.swingcapture;

/** Deterministic contract tests for warm camera transition evidence. */
public final class WarmCaptureTransitionTimingTest {
  private WarmCaptureTransitionTimingTest() {}

  public static void main(String[] arguments) {
    computesEveryTransitionInterval();
    acceptsCoincidentMilestones();
    rejectsNegativeAndOutOfOrderMilestones();
    classifiesExpectedStandbyFlushes();
    normalizesSessionFrameOrdinals();
    estimatesClockOffsetAtMonotonicMidpoint();
  }

  private static void normalizesSessionFrameOrdinals() {
    check(WarmCaptureTransitionTiming.highSpeedOrdinal(67, 67) == 0, "first ordinal");
    check(WarmCaptureTransitionTiming.highSpeedOrdinal(67, 68) == 1, "second ordinal");
    expectThrows(
        () -> WarmCaptureTransitionTiming.highSpeedOrdinal(67, 66),
        "frame before high-speed session");
  }

  private static void estimatesClockOffsetAtMonotonicMidpoint() {
    check(
        WarmCaptureTransitionTiming.midpointClockOffsetNanos(1_000, 201_005, 1_010)
            == 200_000,
        "positive suspend offset");
    check(
        WarmCaptureTransitionTiming.midpointClockOffsetNanos(1_000, 1_004, 1_009) == 0,
        "odd midpoint rounds down");
    expectThrows(
        () -> WarmCaptureTransitionTiming.midpointClockOffsetNanos(2, 4, 1),
        "reversed monotonic anchor");
  }

  private static void classifiesExpectedStandbyFlushes() {
    check(
        WarmCaptureTransitionTiming.standbyCaptureFailureIsFatal(false, false),
        "active standby failure is fatal");
    check(
        !WarmCaptureTransitionTiming.standbyCaptureFailureIsFatal(true, false),
        "transition flush is expected");
    check(
        !WarmCaptureTransitionTiming.standbyCaptureFailureIsFatal(false, true),
        "closed-session flush is expected");
  }

  private static void computesEveryTransitionInterval() {
    WarmCaptureTransitionTiming timing =
        new WarmCaptureTransitionTiming(100, 120, 160, 240, 260, 310);

    check(timing.transitionToEncoderStartNs() == 20, "encoder start");
    check(timing.transitionToStandbySessionClosedNs() == 60, "standby close");
    check(timing.transitionToHighSpeedSessionConfiguredNs() == 140, "session configuration");
    check(timing.transitionToFirstHighSpeedCameraFrameNs() == 160, "camera frame");
    check(timing.transitionToFirstUsableEncodedFrameNs() == 210, "encoded frame");
  }

  private static void acceptsCoincidentMilestones() {
    WarmCaptureTransitionTiming timing = new WarmCaptureTransitionTiming(5, 5, 5, 5, 5, 5);
    check(timing.transitionToFirstUsableEncodedFrameNs() == 0, "coincident milestones");
  }

  private static void rejectsNegativeAndOutOfOrderMilestones() {
    expectThrows(() -> new WarmCaptureTransitionTiming(-1, 0, 0, 0, 0, 0), "negative request");
    expectThrows(() -> new WarmCaptureTransitionTiming(2, 1, 3, 4, 5, 6), "encoder before request");
    expectThrows(() -> new WarmCaptureTransitionTiming(1, 3, 2, 4, 5, 6), "close before encoder");
    expectThrows(() -> new WarmCaptureTransitionTiming(1, 2, 4, 3, 5, 6), "configure before close");
    expectThrows(() -> new WarmCaptureTransitionTiming(1, 2, 3, 5, 4, 6), "frame before config");
    expectThrows(() -> new WarmCaptureTransitionTiming(1, 2, 3, 4, 6, 5), "AU before frame");
  }

  private static void expectThrows(Runnable action, String message) {
    try {
      action.run();
    } catch (IllegalArgumentException expected) {
      return;
    }
    throw new AssertionError(message + " did not throw");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
