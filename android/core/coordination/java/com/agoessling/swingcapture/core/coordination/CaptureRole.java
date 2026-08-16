package com.agoessling.swingcapture.core.coordination;

/** Stable capture roles carried over the wire and in session manifests. */
public enum CaptureRole {
  DOWN_THE_LINE("down_the_line"),
  FACE_ON("face_on");

  private final String wireName;

  CaptureRole(String wireName) {
    this.wireName = wireName;
  }

  public String wireName() {
    return wireName;
  }

  public static CaptureRole parse(String value) {
    for (CaptureRole role : values()) {
      if (role.wireName.equals(value)) {
        return role;
      }
    }
    throw new IllegalArgumentException("Unknown capture role: " + value);
  }
}
