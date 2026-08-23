package com.agoessling.swingcapture;

/** Deterministic regression coverage for terminal field-recorder failure acknowledgement. */
public final class FieldRecordingStopPolicyTest {
  public static void main(String[] ignoredArguments) {
    check(
        FieldRecordingStopPolicy.decide(false, false, false)
            == FieldRecordingStopPolicy.Action.NO_OP,
        "absent recorder stop");
    check(
        FieldRecordingStopPolicy.decide(true, true, false)
            == FieldRecordingStopPolicy.Action.STOP_ACTIVE,
        "active recorder stop");
    check(
        FieldRecordingStopPolicy.decide(true, false, false)
            == FieldRecordingStopPolicy.Action.NO_OP,
        "completed recorder stop");
    check(
        FieldRecordingStopPolicy.decide(true, false, true)
            == FieldRecordingStopPolicy.Action.ACKNOWLEDGE_FAILED,
        "failed recorder acknowledgement");
    expectInvalid(false, true, false);
    expectInvalid(false, false, true);
    expectInvalid(true, true, true);
    check(
        !FieldRecordingStopPolicy.captureFailureIsFatal(true, true),
        "an explicit stop must ignore its flushed Camera2 request");
    check(
        FieldRecordingStopPolicy.captureFailureIsFatal(false, true),
        "an unexpected flush remains fatal");
    check(
        FieldRecordingStopPolicy.captureFailureIsFatal(true, false),
        "a real capture error remains fatal during stop");
    check(
        FieldRecordingStopPolicy.captureFailureIsFatal(false, false),
        "a real active capture error remains fatal");
  }

  private static void expectInvalid(boolean present, boolean active, boolean failed) {
    try {
      FieldRecordingStopPolicy.decide(present, active, failed);
      throw new AssertionError("inconsistent recorder state was accepted");
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }

  private FieldRecordingStopPolicyTest() {}
}
