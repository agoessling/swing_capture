package com.agoessling.swingcapture.pose;

import java.util.Locale;

/** Configured station projection; it is never inferred from the golfer image. */
public enum PoseProjection {
  ACROSS_THE_LINE("atl"),
  DOWN_THE_LINE("dtl");

  private final String wireName;

  PoseProjection(String wireName) {
    this.wireName = wireName;
  }

  public String wireName() {
    return wireName;
  }

  public static PoseProjection parse(String value) {
    if (value == null) {
      throw new IllegalArgumentException("projection is required");
    }
    String normalized = value.toLowerCase(Locale.ROOT);
    for (PoseProjection projection : values()) {
      if (projection.wireName.equals(normalized)) {
        return projection;
      }
    }
    throw new IllegalArgumentException("projection must be atl or dtl");
  }
}
