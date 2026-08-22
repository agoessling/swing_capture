package com.agoessling.swingcapture;

public final class PoseNodeModeTest {
  private PoseNodeModeTest() {}

  public static void main(String[] args) {
    check(PoseNodeMode.parse(null) == PoseNodeMode.DISABLED, "null defaults disabled");
    check(PoseNodeMode.parse("") == PoseNodeMode.DISABLED, "empty defaults disabled");
    for (PoseNodeMode mode : PoseNodeMode.values()) {
      check(PoseNodeMode.parse(mode.wireName()) == mode, "wire round trip");
      check(
          PoseNodeMode.parse("  " + mode.wireName().toUpperCase() + " ") == mode,
          "case-normalized round trip");
      check(!mode.displayName().isBlank(), "display name");
    }
    expectFailure(() -> PoseNodeMode.parse("primary"), "unknown mode");
  }

  private static void expectFailure(Runnable operation, String label) {
    try {
      operation.run();
      throw new AssertionError(label + " did not fail");
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
