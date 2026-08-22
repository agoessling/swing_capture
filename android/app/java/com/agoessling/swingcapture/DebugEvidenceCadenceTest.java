package com.agoessling.swingcapture;

public final class DebugEvidenceCadenceTest {
  private DebugEvidenceCadenceTest() {}

  public static void main(String[] arguments) {
    DebugEvidenceCadence cadence = new DebugEvidenceCadence(1_000);
    check(cadence.shouldSnapshot(0, false), "first evidence");
    check(!cadence.shouldSnapshot(200, false), "five-Hz frame skipped");
    check(!cadence.shouldSnapshot(800, false), "subsecond frame skipped");
    check(cadence.shouldSnapshot(1_000, false), "one-Hz frame retained");
    check(cadence.shouldSnapshot(1_200, true), "arm decision bypasses cadence");
    check(!cadence.shouldSnapshot(1_400, false), "cadence continues after arm evidence");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
