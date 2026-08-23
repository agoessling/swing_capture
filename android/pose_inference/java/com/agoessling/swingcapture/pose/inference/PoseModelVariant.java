package com.agoessling.swingcapture.pose.inference;

import java.util.Locale;

/** Closed model-asset names accepted by the manual pose experiment path. */
public enum PoseModelVariant {
  LITE("lite", "pose_landmarker_lite.task"),
  FULL("full", "pose_landmarker_full.task"),
  HEAVY("heavy", "pose_landmarker_heavy.task");

  private final String wireName;
  private final String assetPath;

  PoseModelVariant(String wireName, String assetPath) {
    this.wireName = wireName;
    this.assetPath = assetPath;
  }

  public String wireName() {
    return wireName;
  }

  public String assetPath() {
    return assetPath;
  }

  public static PoseModelVariant productionDefault() {
    return LITE;
  }

  public static PoseModelVariant parse(String value) {
    if (value == null) {
      throw new IllegalArgumentException("pose model variant is required");
    }
    String normalized = value.trim().toLowerCase(Locale.ROOT);
    for (PoseModelVariant variant : values()) {
      if (variant.wireName.equals(normalized)) {
        return variant;
      }
    }
    throw new IllegalArgumentException("Unknown pose model variant: " + value);
  }
}
