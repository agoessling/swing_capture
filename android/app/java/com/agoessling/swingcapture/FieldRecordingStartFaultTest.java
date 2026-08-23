package com.agoessling.swingcapture;

/** Deterministic coverage for the fail-closed, one-shot field-recording HIL fault. */
public final class FieldRecordingStartFaultTest {
  public static void main(String[] ignoredArguments) {
    disabledByDefault();
    consumesExactlyOneStart();
    acceptedFailureWaitsForPostAcceptanceRelease();
    acceptedFailureReleaseCannotRaceAheadOfTheStart();
    clearingAWaitingAcceptedFailureFailsClosed();
    acceptedFailureTimesOutClosed();
    explicitClearDisablesPendingFault();
  }

  private static void disabledByDefault() {
    FieldRecordingStartFault fault = new FieldRecordingStartFault();
    check(!fault.armed(), "new fault must be disabled");
    fault.rejectIfArmed();
    check(!fault.armed(), "an ordinary start must not arm the fault");
  }

  private static void consumesExactlyOneStart() {
    FieldRecordingStartFault fault = new FieldRecordingStartFault();
    fault.arm();
    check(fault.armed(), "explicit HIL action must arm the fault");
    try {
      fault.rejectIfArmed();
      throw new AssertionError("armed fault must reject the next start");
    } catch (IllegalStateException expected) {
      check(
          FieldRecordingStartFault.REJECTION_MESSAGE.equals(expected.getMessage()),
          "rejection must remain actionable and stable");
    }
    check(!fault.armed(), "rejection must consume the fault");
    fault.rejectIfArmed();
  }

  private static void explicitClearDisablesPendingFault() {
    FieldRecordingStartFault fault = new FieldRecordingStartFault();
    fault.arm();
    fault.clear();
    check(!fault.armed(), "clear must remove the pending fault");
    fault.rejectIfArmed();

    fault.armAcceptedFailure();
    fault.clear();
    check(
        !fault.acceptedFailureArmed(),
        "clear before an accepted start reaches the gate must disable the fault");
    fault.failAcceptedStartIfArmed();
  }

  private static void acceptedFailureWaitsForPostAcceptanceRelease() {
    FieldRecordingStartFault fault = new FieldRecordingStartFault();
    Throwable[] observed = new Throwable[1];
    fault.armAcceptedFailure();
    Thread start =
        new Thread(
            () -> {
              try {
                fault.failAcceptedStartIfArmed();
              } catch (Throwable failure) {
                observed[0] = failure;
              }
            });
    start.start();
    waitUntil(fault::acceptedStartWaiting, "accepted start did not reach its HIL gate");
    check(fault.acceptedFailureArmed(), "waiting accepted-start fault must remain observable");
    check(fault.releaseAcceptedStartFailure(), "post-acceptance release must be accepted");
    join(start);
    check(observed[0] instanceof IllegalStateException, "released start must fail");
    check(
        FieldRecordingStartFault.ACCEPTED_FAILURE_MESSAGE.equals(observed[0].getMessage()),
        "accepted-start failure diagnostic must remain stable");
    check(!fault.acceptedFailureArmed(), "accepted-start failure must be one shot");
    fault.failAcceptedStartIfArmed();
  }

  private static void acceptedFailureReleaseCannotRaceAheadOfTheStart() {
    FieldRecordingStartFault fault = new FieldRecordingStartFault();
    fault.armAcceptedFailure();
    check(
        !fault.releaseAcceptedStartFailure(),
        "the HIL may release only a start that is already waiting after acceptance");
    fault.clear();
  }

  private static void acceptedFailureTimesOutClosed() {
    FieldRecordingStartFault fault = new FieldRecordingStartFault(1);
    fault.armAcceptedFailure();
    try {
      fault.failAcceptedStartIfArmed();
      throw new AssertionError("unreleased accepted-start fault must time out closed");
    } catch (IllegalStateException expected) {
      check(
          FieldRecordingStartFault.ACCEPTED_FAILURE_TIMEOUT_MESSAGE.equals(expected.getMessage()),
          "timeout diagnostic must identify the missing post-acceptance release");
    }
    check(!fault.acceptedFailureArmed(), "timed-out accepted-start fault must be consumed");
  }

  private static void clearingAWaitingAcceptedFailureFailsClosed() {
    FieldRecordingStartFault fault = new FieldRecordingStartFault();
    Throwable[] observed = new Throwable[1];
    fault.armAcceptedFailure();
    Thread start =
        new Thread(
            () -> {
              try {
                fault.failAcceptedStartIfArmed();
              } catch (Throwable failure) {
                observed[0] = failure;
              }
            });
    start.start();
    waitUntil(fault::acceptedStartWaiting, "accepted start did not reach its clear test gate");
    fault.clear();
    join(start);
    check(observed[0] instanceof IllegalStateException, "cleared waiting start must fail closed");
    check(
        FieldRecordingStartFault.ACCEPTED_FAILURE_CLEARED_MESSAGE.equals(
            observed[0].getMessage()),
        "cleared waiting start must have a stable fail-closed diagnostic");
    check(!fault.acceptedFailureArmed(), "cleared waiting start must consume the fault");
  }

  private static void waitUntil(Check condition, String message) {
    long deadline = System.nanoTime() + 2_000_000_000L;
    while (!condition.evaluate() && System.nanoTime() < deadline) {
      Thread.onSpinWait();
    }
    check(condition.evaluate(), message);
  }

  private static void join(Thread thread) {
    try {
      thread.join(2_000);
    } catch (InterruptedException interrupted) {
      Thread.currentThread().interrupt();
      throw new AssertionError("interrupted waiting for accepted-start fault", interrupted);
    }
    check(!thread.isAlive(), "accepted-start fault thread did not terminate");
  }

  @FunctionalInterface
  private interface Check {
    boolean evaluate();
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }

  private FieldRecordingStartFaultTest() {}
}
