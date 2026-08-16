package com.agoessling.swingcapture.core.coordination;

import java.util.Objects;

/** A node-local trigger estimate, before mapping into the coordinator clock. */
public record NodeTriggerReport(
    CaptureRole role,
    String nodeId,
    String sessionId,
    long triggerTimestampNs,
    long timestampUncertaintyNs) {
  public NodeTriggerReport {
    Objects.requireNonNull(role, "role");
    requireIdentifier(nodeId, "nodeId");
    requireIdentifier(sessionId, "sessionId");
    if (triggerTimestampNs < 0) {
      throw new IllegalArgumentException("triggerTimestampNs cannot be negative");
    }
    if (timestampUncertaintyNs < 0) {
      throw new IllegalArgumentException("timestampUncertaintyNs cannot be negative");
    }
  }

  private static void requireIdentifier(String value, String label) {
    Objects.requireNonNull(value, label);
    if (value.isBlank()) {
      throw new IllegalArgumentException(label + " cannot be blank");
    }
  }
}
