package com.agoessling.swingcapture;

/** Deterministic tests for low-rate single-shot camera cadence. */
public final class LowRateCaptureScheduleTest {
  private LowRateCaptureScheduleTest() {}

  public static void main(String[] arguments) {
    check(LowRateCaptureSchedule.nextDelayMillis(1_000, 50_001_000, 200) == 150,
        "fast callback leaves the remainder of the period");
    check(LowRateCaptureSchedule.nextDelayMillis(0, 199_000_001, 200) == 1,
        "delay rounds outward to milliseconds");
    check(LowRateCaptureSchedule.nextDelayMillis(0, 200_000_000, 200) == 0,
        "exact period needs no delay");
    check(LowRateCaptureSchedule.nextDelayMillis(0, 250_000_000, 200) == 0,
        "late callback schedules immediately");
    expectThrows(() -> LowRateCaptureSchedule.nextDelayMillis(2, 1, 200));
    expectThrows(() -> LowRateCaptureSchedule.nextDelayMillis(0, 0, 0));
  }

  private static void expectThrows(Runnable action) {
    try {
      action.run();
    } catch (IllegalArgumentException expected) {
      return;
    }
    throw new AssertionError("invalid schedule did not throw");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
