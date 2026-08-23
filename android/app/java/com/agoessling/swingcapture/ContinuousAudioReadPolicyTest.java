package com.agoessling.swingcapture;

public final class ContinuousAudioReadPolicyTest {
  private ContinuousAudioReadPolicyTest() {}

  public static void main(String[] args) {
    check(
        ContinuousAudioReadPolicy.framesPerRead(48_000) == 960,
        "48 kHz reads must be bounded to 20 ms");
    check(
        ContinuousAudioReadPolicy.framesPerRead(44_100) == 882,
        "integral alternate sample rate");
    check(
        ContinuousAudioReadPolicy.framesPerRead(1) == 1,
        "sub-frame duration rounds up");
    expectIllegalArgument(() -> ContinuousAudioReadPolicy.framesPerRead(0));
    expectIllegalArgument(() -> ContinuousAudioReadPolicy.framesPerRead(-1));
  }

  private static void expectIllegalArgument(Runnable action) {
    try {
      action.run();
      throw new AssertionError("expected IllegalArgumentException");
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
