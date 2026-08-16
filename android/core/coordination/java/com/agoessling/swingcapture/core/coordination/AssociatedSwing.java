package com.agoessling.swingcapture.core.coordination;

import java.util.Objects;

/** Exactly one accepted down-the-line/face-on pair for a shared capture session. */
public record AssociatedSwing(
    String sessionId,
    ObservedNodeTrigger downTheLine,
    ObservedNodeTrigger faceOn,
    long minimumTriggerSeparationNs,
    long maximumTriggerSeparationNs) {
  public AssociatedSwing {
    Objects.requireNonNull(sessionId, "sessionId");
    Objects.requireNonNull(downTheLine, "downTheLine");
    Objects.requireNonNull(faceOn, "faceOn");
    if (!sessionId.equals(downTheLine.report().sessionId())
        || !sessionId.equals(faceOn.report().sessionId())) {
      throw new IllegalArgumentException("associated reports must share the swing session");
    }
    if (downTheLine.report().role() != CaptureRole.DOWN_THE_LINE
        || faceOn.report().role() != CaptureRole.FACE_ON) {
      throw new IllegalArgumentException("associated reports are assigned to the wrong roles");
    }
    if (minimumTriggerSeparationNs < 0
        || maximumTriggerSeparationNs < minimumTriggerSeparationNs) {
      throw new IllegalArgumentException("trigger separation bounds are inconsistent");
    }
  }
}
