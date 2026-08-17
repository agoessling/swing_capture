package com.agoessling.swingcapture;

/** Regression coverage for encoder discontinuities observed while warming a Pixel pipeline. */
public final class CaptureStartupContinuityPolicyTest {
  private CaptureStartupContinuityPolicyTest() {}

  public static void main(String[] arguments) {
    check(
        CaptureStartupContinuityPolicy.action(true, false, false)
            == CaptureStartupContinuityPolicy.Action.RESET_WARMUP_AND_CONTINUE,
        "idle warmup gap resets");
    check(
        CaptureStartupContinuityPolicy.action(true, true, false)
            == CaptureStartupContinuityPolicy.Action.FAIL_CAPTURE,
        "armed gap fails");
    check(
        CaptureStartupContinuityPolicy.action(true, false, true)
            == CaptureStartupContinuityPolicy.Action.FAIL_CAPTURE,
        "active capture gap fails before readiness");
    check(
        CaptureStartupContinuityPolicy.action(true, true, true)
            == CaptureStartupContinuityPolicy.Action.FAIL_CAPTURE,
        "active armed gap fails");
    check(
        CaptureStartupContinuityPolicy.action(false, false, false)
            == CaptureStartupContinuityPolicy.Action.FAIL_CAPTURE,
        "non-gap retention failures remain fatal during warmup");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
