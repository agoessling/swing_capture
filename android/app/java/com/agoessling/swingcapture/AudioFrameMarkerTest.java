package com.agoessling.swingcapture;

import java.util.Optional;
import java.util.concurrent.TimeUnit;

/** Deterministic BOOTTIME-to-AudioRecord frame mapping boundaries. */
public final class AudioFrameMarkerTest {
  private AudioFrameMarkerTest() {}

  public static void main(String[] arguments) {
    mapsFreshTimestampWithOutwardUncertainty();
    rejectsStaleAndFutureTimestamps();
    rejectsInvalidAndOverflowingEvidence();
  }

  private static void mapsFreshTimestampWithOutwardUncertainty() {
    Optional<AudioFrameMarker.Marker> marker =
        AudioFrameMarker.estimate(
            10_250_000_000L, 48_000, 10_000_000_000L, 250_001, 48_000);
    check(marker.isPresent(), "fresh marker");
    check(marker.orElseThrow().framePosition() == 60_000, "quarter-second frame mapping");
    check(marker.orElseThrow().uncertaintyFrames() == 14, "ceil uncertainty plus rounding frame");

    AudioFrameMarker.Marker halfFrameRoundsUp =
        AudioFrameMarker.estimate(10_000_010_417L, 7, 10_000_000_000L, 0, 48_000)
            .orElseThrow();
    check(halfFrameRoundsUp.framePosition() == 8, "half-frame rounds to nearest frame");
    check(halfFrameRoundsUp.uncertaintyFrames() == 1, "rounding uncertainty remains explicit");
  }

  private static void rejectsStaleAndFutureTimestamps() {
    check(
        AudioFrameMarker.estimate(
                TimeUnit.SECONDS.toNanos(11),
                0,
                TimeUnit.SECONDS.toNanos(10),
                0,
                48_000)
            .isEmpty(),
        "stale timestamp");
    check(
        AudioFrameMarker.estimate(9, 0, 10, 0, 48_000).isEmpty(),
        "future timestamp");
  }

  private static void rejectsInvalidAndOverflowingEvidence() {
    expectInvalid(() -> AudioFrameMarker.estimate(-1, 0, 0, 0, 48_000), "negative request");
    check(
        AudioFrameMarker.estimate(20_833, Long.MAX_VALUE, 0, 0, 48_000).isEmpty(),
        "frame position overflow");
  }

  private static void expectInvalid(Action action, String label) {
    try {
      action.run();
      throw new AssertionError("Expected invalid input: " + label);
    } catch (IllegalArgumentException expected) {
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
