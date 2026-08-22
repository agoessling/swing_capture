package com.agoessling.swingcapture;

import java.util.Locale;

/** How this node participates in low-rate pose triggering. */
public enum PoseNodeMode {
  DISABLED("disabled", "Pose monitoring disabled"),
  SHADOW("shadow", "Shadow inference (record decisions only)"),
  LEADER("leader", "Pose leader (arms this node and its configured peer)");

  private final String wireName;
  private final String displayName;

  PoseNodeMode(String wireName, String displayName) {
    this.wireName = wireName;
    this.displayName = displayName;
  }

  public String wireName() {
    return wireName;
  }

  public String displayName() {
    return displayName;
  }

  public static PoseNodeMode parse(String value) {
    if (value == null || value.isBlank()) {
      return DISABLED;
    }
    String normalized = value.trim().toLowerCase(Locale.ROOT);
    for (PoseNodeMode mode : values()) {
      if (mode.wireName.equals(normalized)) {
        return mode;
      }
    }
    throw new IllegalArgumentException("Unknown pose node mode: " + value);
  }
}
