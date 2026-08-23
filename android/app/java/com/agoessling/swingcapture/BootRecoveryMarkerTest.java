package com.agoessling.swingcapture;

/** Boundary coverage for the device-protected boot-marker transition. */
public final class BootRecoveryMarkerTest {
  private BootRecoveryMarkerTest() {}

  public static void main(String[] arguments) {
    BootRecoveryMarker.Observation first =
        BootRecoveryMarker.recordedAfter(0, "locked_boot_completed", 1_789_000_000_000L, 0);
    check(first.observationCount() == 1, "first observation increments from zero");
    check(first.lastEvent().equals("locked_boot_completed"), "event is retained exactly");
    check(first.lastObservedEpochMillis() == 1_789_000_000_000L, "wall time is retained");
    check(first.lastObservedElapsedRealtimeNanos() == 0, "boot elapsed time may be zero");

    BootRecoveryMarker.Observation later =
        BootRecoveryMarker.recordedAfter(
            first.observationCount(), "user_unlocked", 1_789_000_001_000L, 2_000_000_000L);
    check(later.observationCount() == 2, "subsequent observation increments exactly once");
    check(later.lastEvent().equals("user_unlocked"), "latest event replaces the prior event");

    BootRecoveryMarker.validateStored(0, "", 0, 0);
    BootRecoveryMarker.validateStored(
        later.observationCount(),
        later.lastEvent(),
        later.lastObservedEpochMillis(),
        later.lastObservedElapsedRealtimeNanos());

    expectFailure(
        IllegalStateException.class,
        () -> BootRecoveryMarker.recordedAfter(-1, "boot_completed", 1, 0),
        "corrupt negative counter fails closed");
    expectFailure(
        IllegalStateException.class,
        () -> BootRecoveryMarker.recordedAfter(Long.MAX_VALUE, "boot_completed", 1, 0),
        "exhausted counter fails closed");
    expectFailure(
        IllegalArgumentException.class,
        () -> BootRecoveryMarker.recordedAfter(0, null, 1, 0),
        "null event is rejected");
    expectFailure(
        IllegalArgumentException.class,
        () -> BootRecoveryMarker.recordedAfter(0, "  ", 1, 0),
        "blank event is rejected");
    expectFailure(
        IllegalArgumentException.class,
        () -> BootRecoveryMarker.recordedAfter(0, "boot_completed", 0, 0),
        "missing wall time is rejected");
    expectFailure(
        IllegalArgumentException.class,
        () -> BootRecoveryMarker.recordedAfter(0, "boot_completed", 1, -1),
        "negative elapsed time is rejected");
    expectFailure(
        IllegalArgumentException.class,
        () -> BootRecoveryMarker.validateStored(0, "boot_completed", 0, 0),
        "unobserved marker cannot retain an event");
    expectFailure(
        IllegalArgumentException.class,
        () -> BootRecoveryMarker.validateStored(0, "", 1, 0),
        "unobserved marker cannot retain timestamps");
    expectFailure(
        IllegalArgumentException.class,
        () -> BootRecoveryMarker.validateStored(1, "", 1, 0),
        "observed marker requires an event");
    expectFailure(
        IllegalArgumentException.class,
        () -> BootRecoveryMarker.validateStored(1, "boot_completed", 0, 0),
        "observed marker requires wall time");
  }

  private static void expectFailure(
      Class<? extends RuntimeException> expected, Runnable action, String message) {
    try {
      action.run();
    } catch (RuntimeException failure) {
      check(expected.isInstance(failure), message + " with the expected failure type");
      return;
    }
    throw new AssertionError(message);
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
