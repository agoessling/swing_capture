package com.agoessling.swingcapture;

/** Deterministic contract tests for high-speed startup milestone evidence. */
public final class CaptureStartupTimingTest {
  private CaptureStartupTimingTest() {}

  public static void main(String[] arguments) {
    computesEveryStartupInterval();
    acceptsCoincidentMilestones();
    rejectsNegativeAndOutOfOrderMilestones();
  }

  private static void computesEveryStartupInterval() {
    CaptureStartupTiming timing = new CaptureStartupTiming(100, 120, 210, 260, 2_800);

    check(timing.armToEngineStartNs() == 20, "arm to engine");
    check(timing.engineStartToFirstCameraFrameNs() == 90, "engine to camera");
    check(timing.armToFirstCameraFrameNs() == 110, "arm to camera");
    check(timing.firstCameraFrameToFirstUsableEncodedFrameNs() == 50, "camera to encoder");
    check(timing.armToFirstUsableEncodedFrameNs() == 160, "arm to encoder");
    check(timing.armToFullPreRollReadyNs() == 2_700, "arm to full pre-roll");
    check(
        timing.firstUsableEncodedFrameToFullPreRollReadyNs() == 2_540,
        "encoded frame to full pre-roll");
  }

  private static void acceptsCoincidentMilestones() {
    CaptureStartupTiming timing = new CaptureStartupTiming(5, 5, 5, 5, 5);
    check(timing.armToFullPreRollReadyNs() == 0, "coincident milestones");
  }

  private static void rejectsNegativeAndOutOfOrderMilestones() {
    expectThrows(() -> new CaptureStartupTiming(-1, 0, 0, 0, 0), "negative arm");
    expectThrows(() -> new CaptureStartupTiming(2, 1, 3, 4, 5), "engine before arm");
    expectThrows(() -> new CaptureStartupTiming(1, 3, 2, 4, 5), "camera before engine");
    expectThrows(() -> new CaptureStartupTiming(1, 2, 4, 3, 5), "encoder before camera");
    expectThrows(() -> new CaptureStartupTiming(1, 2, 3, 5, 4), "pre-roll before encoder");
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
