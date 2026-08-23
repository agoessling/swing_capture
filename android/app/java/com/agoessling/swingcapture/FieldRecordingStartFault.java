package com.agoessling.swingcapture;

/** In-memory, one-shot field-recording start faults used only by explicit HIL launches. */
final class FieldRecordingStartFault {
  static final String REJECTION_MESSAGE = "HIL injected field-recording start rejection";
  static final String ACCEPTED_FAILURE_MESSAGE =
      "HIL injected asynchronous field-recording start failure after acceptance";
  static final String ACCEPTED_FAILURE_TIMEOUT_MESSAGE =
      "HIL asynchronous field-recording start failure was not released after acceptance";
  static final String ACCEPTED_FAILURE_CLEARED_MESSAGE =
      "HIL asynchronous field-recording start failure was cleared while waiting";
  private static final long DEFAULT_RELEASE_TIMEOUT_MILLIS = 2_000;

  private enum State {
    NONE,
    REJECT_ARMED,
    ACCEPTED_FAILURE_ARMED,
    ACCEPTED_FAILURE_WAITING,
    ACCEPTED_FAILURE_RELEASED,
    ACCEPTED_FAILURE_CLEARED,
  }

  private final long releaseTimeoutMillis;
  private State state = State.NONE;

  FieldRecordingStartFault() {
    this(DEFAULT_RELEASE_TIMEOUT_MILLIS);
  }

  FieldRecordingStartFault(long releaseTimeoutMillis) {
    if (releaseTimeoutMillis <= 0) {
      throw new IllegalArgumentException("release timeout must be positive");
    }
    this.releaseTimeoutMillis = releaseTimeoutMillis;
  }

  synchronized void arm() {
    state = State.REJECT_ARMED;
  }

  synchronized void armAcceptedFailure() {
    state = State.ACCEPTED_FAILURE_ARMED;
  }

  synchronized void rejectIfArmed() {
    if (state != State.REJECT_ARMED) {
      return;
    }
    state = State.NONE;
    throw new IllegalStateException(REJECTION_MESSAGE);
  }

  synchronized boolean armed() {
    return state == State.REJECT_ARMED;
  }

  synchronized boolean acceptedFailureArmed() {
    return state == State.ACCEPTED_FAILURE_ARMED
        || state == State.ACCEPTED_FAILURE_WAITING;
  }

  synchronized boolean acceptedStartWaiting() {
    return state == State.ACCEPTED_FAILURE_WAITING;
  }

  synchronized boolean releaseAcceptedStartFailure() {
    if (state != State.ACCEPTED_FAILURE_WAITING) {
      return false;
    }
    state = State.ACCEPTED_FAILURE_RELEASED;
    notifyAll();
    return true;
  }

  synchronized void failAcceptedStartIfArmed() {
    if (state != State.ACCEPTED_FAILURE_ARMED) {
      return;
    }
    state = State.ACCEPTED_FAILURE_WAITING;
    notifyAll();
    long remainingNanos = releaseTimeoutMillis * 1_000_000L;
    long previous = System.nanoTime();
    while (state == State.ACCEPTED_FAILURE_WAITING && remainingNanos > 0) {
      try {
        long millis = remainingNanos / 1_000_000L;
        int nanos = (int) (remainingNanos % 1_000_000L);
        wait(millis, nanos);
      } catch (InterruptedException interrupted) {
        state = State.NONE;
        Thread.currentThread().interrupt();
        throw new IllegalStateException(
            "Interrupted awaiting accepted field-recording HIL failure", interrupted);
      }
      long now = System.nanoTime();
      remainingNanos -= Math.max(0, now - previous);
      previous = now;
    }
    boolean released = state == State.ACCEPTED_FAILURE_RELEASED;
    boolean cleared = state == State.ACCEPTED_FAILURE_CLEARED;
    state = State.NONE;
    throw new IllegalStateException(
        released
            ? ACCEPTED_FAILURE_MESSAGE
            : cleared ? ACCEPTED_FAILURE_CLEARED_MESSAGE : ACCEPTED_FAILURE_TIMEOUT_MESSAGE);
  }

  synchronized void clear() {
    state =
        state == State.ACCEPTED_FAILURE_WAITING
            ? State.ACCEPTED_FAILURE_CLEARED
            : State.NONE;
    notifyAll();
  }
}
