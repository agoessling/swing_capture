package com.agoessling.swingcapture;

/** Boundary coverage for startup clipping, post-roll wait, and delayed-report eviction. */
public final class DiagnosticAudioWindowTest {
  private DiagnosticAudioWindowTest() {}

  public static void main(String[] arguments) {
    selectsFullAndStartupClippedWindows();
    reportsWaitAndEvictionWithoutInventingEvidence();
    rejectsInvalidAndOverflowingInput();
  }

  private static void selectsFullAndStartupClippedWindows() {
    DiagnosticAudioWindow.Selection full =
        DiagnosticAudioWindow.select(600, 0, 801, 500, 200);
    check(full.state() == DiagnosticAudioWindow.State.AVAILABLE, "full window available");
    check(full.firstFramePosition() == 100 && full.endFramePosition() == 800, "full range");

    DiagnosticAudioWindow.Selection clipped =
        DiagnosticAudioWindow.select(600, 450, 800, 500, 200);
    check(clipped.state() == DiagnosticAudioWindow.State.AVAILABLE, "clipped window available");
    check(clipped.firstFramePosition() == 450, "startup history clipped honestly");
  }

  private static void reportsWaitAndEvictionWithoutInventingEvidence() {
    check(
        DiagnosticAudioWindow.select(600, -1, -1, 500, 200).state()
            == DiagnosticAudioWindow.State.WAITING_FOR_POST_ROLL,
        "empty ring waits");
    check(
        DiagnosticAudioWindow.select(600, 0, 799, 500, 200).state()
            == DiagnosticAudioWindow.State.WAITING_FOR_POST_ROLL,
        "exclusive end must cover post-roll");
    check(
        DiagnosticAudioWindow.select(600, 601, 1_000, 500, 200).state()
            == DiagnosticAudioWindow.State.MARKER_EVICTED,
        "evicted marker rejected");
  }

  private static void rejectsInvalidAndOverflowingInput() {
    expectInvalid(() -> DiagnosticAudioWindow.select(-1, 0, 1, 0, 1), "negative marker");
    expectInvalid(
        () -> DiagnosticAudioWindow.select(Long.MAX_VALUE, 0, Long.MAX_VALUE, 0, 1),
        "end overflow");
  }

  private static void expectInvalid(Action action, String label) {
    try {
      action.run();
      throw new AssertionError("Expected invalid input: " + label);
    } catch (IllegalArgumentException | ArithmeticException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String label) {
    if (!condition) {
      throw new AssertionError(label);
    }
  }

  @FunctionalInterface
  private interface Action {
    void run();
  }
}
