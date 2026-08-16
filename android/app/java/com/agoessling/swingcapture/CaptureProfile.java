package com.agoessling.swingcapture;

/** Explicit capture profiles qualified by the physical-device HIL. */
public enum CaptureProfile {
  HD_240(
      "720p240",
      "Standard: 1280×720 / 240 fps / H.264 12 Mbit/s",
      1280,
      720,
      12_000_000),
  FULL_HD_240(
      "1080p240",
      "Optional: 1920×1080 / 240 fps / H.264 24 Mbit/s",
      1920,
      1080,
      24_000_000);

  private final String wireName;
  private final String displayName;
  private final int width;
  private final int height;
  private final int bitrateBitsPerSecond;

  CaptureProfile(
      String wireName,
      String displayName,
      int width,
      int height,
      int bitrateBitsPerSecond) {
    this.wireName = wireName;
    this.displayName = displayName;
    this.width = width;
    this.height = height;
    this.bitrateBitsPerSecond = bitrateBitsPerSecond;
  }

  public String wireName() {
    return wireName;
  }

  public String displayName() {
    return displayName;
  }

  public int width() {
    return width;
  }

  public int height() {
    return height;
  }

  public int bitrateBitsPerSecond() {
    return bitrateBitsPerSecond;
  }

  public static CaptureProfile standard() {
    return HD_240;
  }

  public static CaptureProfile parse(String value) {
    if (value == null || value.isBlank()) {
      return standard();
    }
    for (CaptureProfile profile : values()) {
      if (profile.wireName.equals(value)) {
        return profile;
      }
    }
    throw new IllegalArgumentException("Unknown capture profile " + value);
  }
}
