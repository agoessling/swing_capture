package com.agoessling.swingcapture.pose.inference;

/** Deterministic full-range-safe BT.601 I420 to ARGB conversion. */
public final class I420ToRgb {
  private I420ToRgb() {}

  public static RgbFrame convert(I420Frame frame) {
    int width = frame.width();
    int height = frame.height();
    int chromaWidth = (width + 1) / 2;
    int[] argb = new int[width * height];
    for (int row = 0; row < height; row++) {
      for (int column = 0; column < width; column++) {
        int y = Byte.toUnsignedInt(frame.y()[row * width + column]);
        int chromaIndex = (row / 2) * chromaWidth + column / 2;
        int u = Byte.toUnsignedInt(frame.u()[chromaIndex]);
        int v = Byte.toUnsignedInt(frame.v()[chromaIndex]);

        int c = Math.max(0, y - 16);
        int d = u - 128;
        int e = v - 128;
        int red = clamp((298 * c + 409 * e + 128) >> 8);
        int green = clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
        int blue = clamp((298 * c + 516 * d + 128) >> 8);
        argb[row * width + column] =
            0xff000000 | (red << 16) | (green << 8) | blue;
      }
    }
    return new RgbFrame(width, height, frame.timestampNs(), argb);
  }

  private static int clamp(int value) {
    return Math.min(255, Math.max(0, value));
  }
}
