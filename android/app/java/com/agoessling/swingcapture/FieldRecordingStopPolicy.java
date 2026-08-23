package com.agoessling.swingcapture;

/** Pure policy for an explicit stop across active and terminal field-recorder states. */
final class FieldRecordingStopPolicy {
  enum Action {
    NO_OP,
    STOP_ACTIVE,
    ACKNOWLEDGE_FAILED
  }

  static Action decide(boolean recorderPresent, boolean active, boolean failed) {
    if (!recorderPresent) {
      if (active || failed) {
        throw new IllegalArgumentException("absent recorder cannot be active or failed");
      }
      return Action.NO_OP;
    }
    if (active && failed) {
      throw new IllegalArgumentException("recorder cannot be active and terminally failed");
    }
    if (active) {
      return Action.STOP_ACTIVE;
    }
    return failed ? Action.ACKNOWLEDGE_FAILED : Action.NO_OP;
  }

  static boolean captureFailureIsFatal(boolean stopRequested, boolean flushed) {
    return !stopRequested || !flushed;
  }

  private FieldRecordingStopPolicy() {}
}
