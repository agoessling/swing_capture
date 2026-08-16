package com.agoessling.swingcapture.node;

import java.util.Objects;

/** Thread-safe shared-session and latest-trigger state exposed by one capture node. */
public final class NodeCoordinationState {
  public record TriggerReport(
      String role,
      String nodeId,
      String sharedSessionId,
      String localSessionId,
      long triggerElapsedRealtimeNanos,
      long timestampUncertaintyNanos,
      String source) {
    public TriggerReport {
      requireText(role, "role");
      requireText(nodeId, "nodeId");
      validateSharedSessionId(sharedSessionId);
      requireText(localSessionId, "localSessionId");
      if (triggerElapsedRealtimeNanos <= 0) {
        throw new IllegalArgumentException("trigger timestamp must be positive");
      }
      if (timestampUncertaintyNanos < 0) {
        throw new IllegalArgumentException("timestamp uncertainty cannot be negative");
      }
      requireText(source, "source");
    }
  }

  private String sharedSessionId;
  private TriggerReport latestTrigger;

  public synchronized void armed(String sharedSessionId) {
    if (sharedSessionId != null) {
      validateSharedSessionId(sharedSessionId);
    }
    this.sharedSessionId = sharedSessionId;
    latestTrigger = null;
  }

  public synchronized String sharedSessionId() {
    return sharedSessionId;
  }

  public synchronized void triggered(
      String role,
      String nodeId,
      String localSessionId,
      long triggerElapsedRealtimeNanos,
      long timestampUncertaintyNanos,
      String source) {
    if (sharedSessionId == null) {
      return;
    }
    latestTrigger =
        new TriggerReport(
            role,
            nodeId,
            sharedSessionId,
            localSessionId,
            triggerElapsedRealtimeNanos,
            timestampUncertaintyNanos,
            source);
  }

  public synchronized TriggerReport latestTrigger() {
    return latestTrigger;
  }

  public static void validateSharedSessionId(String sharedSessionId) {
    Objects.requireNonNull(sharedSessionId, "sharedSessionId");
    if (sharedSessionId.isBlank() || sharedSessionId.length() > 128) {
      throw new IllegalArgumentException("sharedSessionId must contain 1 to 128 characters");
    }
    for (int index = 0; index < sharedSessionId.length(); ++index) {
      char value = sharedSessionId.charAt(index);
      boolean valid =
          (value >= 'a' && value <= 'z')
              || (value >= 'A' && value <= 'Z')
              || (value >= '0' && value <= '9')
              || value == '-'
              || value == '_'
              || value == '.';
      if (!valid) {
        throw new IllegalArgumentException(
            "sharedSessionId may contain only ASCII letters, digits, '.', '_', and '-'");
      }
    }
  }

  private static void requireText(String value, String name) {
    if (value == null || value.isBlank()) {
      throw new IllegalArgumentException(name + " is required");
    }
  }
}
