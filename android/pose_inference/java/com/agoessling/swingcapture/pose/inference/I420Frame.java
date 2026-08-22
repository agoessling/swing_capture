package com.agoessling.swingcapture.pose.inference;

import java.util.Objects;

/** Tightly packed planar YUV 4:2:0 frame used at the Android/host-test conversion boundary. */
public record I420Frame(
    int width, int height, long timestampNs, byte[] y, byte[] u, byte[] v) {
  public I420Frame {
    if (width <= 0 || height <= 0) {
      throw new IllegalArgumentException("frame dimensions must be positive");
    }
    if (timestampNs < 0) {
      throw new IllegalArgumentException("timestampNs cannot be negative");
    }
    Objects.requireNonNull(y, "y");
    Objects.requireNonNull(u, "u");
    Objects.requireNonNull(v, "v");
    int chromaWidth = (width + 1) / 2;
    int chromaHeight = (height + 1) / 2;
    if (y.length != width * height
        || u.length != chromaWidth * chromaHeight
        || v.length != chromaWidth * chromaHeight) {
      throw new IllegalArgumentException("plane lengths do not match dimensions");
    }
  }
}
