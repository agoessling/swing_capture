package com.agoessling.swingcapture;

import java.util.Locale;

/** Stable camera roles used by the API and session manifests. */
public enum CaptureRole {
  UNASSIGNED("unassigned", "Unassigned"),
  DOWN_THE_LINE("down_the_line", "Down the line"),
  FACE_ON("face_on", "Across the line (face-on)");

  private final String wireName;
  private final String displayName;

  CaptureRole(String wireName, String displayName) {
    this.wireName = wireName;
    this.displayName = displayName;
  }

  public String wireName() {
    return wireName;
  }

  public String displayName() {
    return displayName;
  }

  public static CaptureRole parse(String value) {
    if (value == null) {
      return UNASSIGNED;
    }
    String normalized = value.trim().toLowerCase(Locale.ROOT);
    for (CaptureRole role : values()) {
      if (role.wireName.equals(normalized)) {
        return role;
      }
    }
    throw new IllegalArgumentException("Unknown capture role: " + value);
  }
}
