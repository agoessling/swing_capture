package com.agoessling.swingcapture;

import java.nio.ByteBuffer;
import java.util.Objects;

/** Pure stride-aware conversion from planar YUV_420_888 samples to NV21 byte order. */
final class Yuv420PlanePacker {
  record Plane(ByteBuffer buffer, int offset, int rowStride, int pixelStride) {
    Plane {
      buffer = Objects.requireNonNull(buffer, "buffer").asReadOnlyBuffer();
      if (offset < 0 || offset > buffer.limit() || rowStride <= 0 || pixelStride <= 0) {
        throw new IllegalArgumentException("invalid plane offset or stride");
      }
    }
  }

  private Yuv420PlanePacker() {}

  static byte[] toNv21(int width, int height, Plane y, Plane u, Plane v) {
    requirePlanes(width, height, y, u, v);

    byte[] nv21 = new byte[Math.multiplyExact(width, height) * 3 / 2];
    int output = 0;
    for (int row = 0; row < height; row++) {
      int input = y.offset() + row * y.rowStride();
      for (int column = 0; column < width; column++) {
        nv21[output++] = y.buffer().get(input + column * y.pixelStride());
      }
    }
    int chromaWidth = width / 2;
    int chromaHeight = height / 2;
    for (int row = 0; row < chromaHeight; row++) {
      int uInput = u.offset() + row * u.rowStride();
      int vInput = v.offset() + row * v.rowStride();
      for (int column = 0; column < chromaWidth; column++) {
        nv21[output++] = v.buffer().get(vInput + column * v.pixelStride());
        nv21[output++] = u.buffer().get(uInput + column * u.pixelStride());
      }
    }
    return nv21;
  }

  /** Converts Camera2 limited-range BT.601 YUV directly to owned Android ARGB pixels. */
  static int[] toArgb(int width, int height, Plane y, Plane u, Plane v) {
    requirePlanes(width, height, y, u, v);
    int[] argb = new int[Math.multiplyExact(width, height)];
    int output = 0;
    for (int row = 0; row < height; ++row) {
      int yInput = y.offset() + row * y.rowStride();
      int uInput = u.offset() + (row / 2) * u.rowStride();
      int vInput = v.offset() + (row / 2) * v.rowStride();
      for (int column = 0; column < width; ++column) {
        int luminance = (y.buffer().get(yInput + column * y.pixelStride()) & 0xff) - 16;
        int blueDifference =
            (u.buffer().get(uInput + (column / 2) * u.pixelStride()) & 0xff) - 128;
        int redDifference =
            (v.buffer().get(vInput + (column / 2) * v.pixelStride()) & 0xff) - 128;
        int scaledY = 298 * Math.max(0, luminance);
        int red = clampByte((scaledY + 409 * redDifference + 128) >> 8);
        int green =
            clampByte((scaledY - 100 * blueDifference - 208 * redDifference + 128) >> 8);
        int blue = clampByte((scaledY + 516 * blueDifference + 128) >> 8);
        argb[output++] = 0xff000000 | (red << 16) | (green << 8) | blue;
      }
    }
    return argb;
  }

  private static void requirePlanes(int width, int height, Plane y, Plane u, Plane v) {
    if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0) {
      throw new IllegalArgumentException("YUV_420 dimensions must be positive and even");
    }
    Objects.requireNonNull(y, "y");
    Objects.requireNonNull(u, "u");
    Objects.requireNonNull(v, "v");
    requireAddressable(y, width, height, "Y");
    int chromaWidth = width / 2;
    int chromaHeight = height / 2;
    requireAddressable(u, chromaWidth, chromaHeight, "U");
    requireAddressable(v, chromaWidth, chromaHeight, "V");

  }

  private static int clampByte(int value) {
    return Math.min(255, Math.max(0, value));
  }

  private static void requireAddressable(Plane plane, int columns, int rows, String name) {
    long last =
        (long) plane.offset()
            + (long) (rows - 1) * plane.rowStride()
            + (long) (columns - 1) * plane.pixelStride();
    if (last < 0 || last >= plane.buffer().limit()) {
      throw new IllegalArgumentException(name + " plane strides exceed its buffer");
    }
  }
}
