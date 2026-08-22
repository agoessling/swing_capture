package com.agoessling.swingcapture.pose.inference;

import java.util.Objects;

/** An owned ARGB inference frame timestamped on the camera's monotonic clock. */
public final class RgbFrame {
  private final int width;
  private final int height;
  private final long timestampNs;
  private final int[] argb;

  /** Constructs a frame and transfers ownership of {@code argb} to this frame. */
  public RgbFrame(int width, int height, long timestampNs, int[] argb) {
    if (width <= 0 || height <= 0) {
      throw new IllegalArgumentException("frame dimensions must be positive");
    }
    if (timestampNs < 0) {
      throw new IllegalArgumentException("timestampNs cannot be negative");
    }
    this.argb = Objects.requireNonNull(argb, "argb");
    if ((long) width * height != argb.length) {
      throw new IllegalArgumentException("argb length does not match dimensions");
    }
    this.width = width;
    this.height = height;
    this.timestampNs = timestampNs;
  }

  public int width() {
    return width;
  }

  public int height() {
    return height;
  }

  public long timestampNs() {
    return timestampNs;
  }

  /** Returns the owned pixels. Consumers must treat this array as read-only. */
  public int[] argb() {
    return argb;
  }
}
