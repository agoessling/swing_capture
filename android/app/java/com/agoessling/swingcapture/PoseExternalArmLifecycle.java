package com.agoessling.swingcapture;

import com.agoessling.swingcapture.pose.PoseTriggerController;
import java.util.Objects;
import java.util.function.LongSupplier;

/**
 * Enforces that a claimed external arm drains standby inference before advancing its controller.
 */
final class PoseExternalArmLifecycle {
  enum State {
    CLAIMED,
    INFERENCE_QUIESCED,
    CONTROLLER_STARTED
  }

  private State state = State.CLAIMED;

  synchronized void inferenceQuiesced() {
    if (state != State.CLAIMED) {
      throw new IllegalStateException("external pose arm can quiesce inference only once");
    }
    state = State.INFERENCE_QUIESCED;
  }

  synchronized PoseTriggerController.Decision startController(
      PoseTriggerControllerLease lease, LongSupplier timestampSource) {
    Objects.requireNonNull(lease, "lease");
    Objects.requireNonNull(timestampSource, "timestampSource");
    if (state != State.INFERENCE_QUIESCED) {
      throw new IllegalStateException(
          "external pose arm cannot advance the controller before inference is quiescent");
    }
    PoseTriggerController.Decision decision = lease.externalCaptureStarted(timestampSource);
    state = State.CONTROLLER_STARTED;
    return decision;
  }

  synchronized State state() {
    return state;
  }
}
