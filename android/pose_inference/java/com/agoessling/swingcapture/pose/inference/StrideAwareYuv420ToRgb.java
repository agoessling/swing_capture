package com.agoessling.swingcapture.pose.inference;

import java.nio.ByteBuffer;
import java.util.Objects;

/** One-pass stride-aware limited-range BT.601 YUV420 to ARGB/RGBA conversion. */
public final class StrideAwareYuv420ToRgb {
  public record Plane(ByteBuffer buffer, int offset, int rowStride, int pixelStride) {
    public Plane {
      buffer = Objects.requireNonNull(buffer, "buffer").asReadOnlyBuffer();
      if (offset < 0 || offset > buffer.limit() || rowStride <= 0 || pixelStride <= 0) {
        throw new IllegalArgumentException("invalid YUV plane offset or stride");
      }
    }
  }

  private StrideAwareYuv420ToRgb() {}

  public static RgbFrame convert(
      int width, int height, long timestampNs, Plane y, Plane u, Plane v) {
    if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0) {
      throw new IllegalArgumentException("YUV420 dimensions must be positive and even");
    }
    requireAddressable(y, width, height, "Y");
    requireAddressable(u, width / 2, height / 2, "U");
    requireAddressable(v, width / 2, height / 2, "V");

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
        int red = clamp((scaledY + 409 * redDifference + 128) >> 8);
        int green =
            clamp((scaledY - 100 * blueDifference - 208 * redDifference + 128) >> 8);
        int blue = clamp((scaledY + 516 * blueDifference + 128) >> 8);
        argb[output++] = 0xff000000 | (red << 16) | (green << 8) | blue;
      }
    }
    return new RgbFrame(width, height, timestampNs, argb);
  }

  /**
   * Writes row-aligned RGB bytes into a caller-owned direct buffer without allocating pixels.
   *
   * <p>This is the production Camera2-to-MediaPipe boundary. MediaPipe Tasks 0.10.35 does not
   * accept a YUV {@code android.media.Image} in its Android packet creator. Its three-channel
   * byte-buffer path requires four-byte row alignment and synchronously copies the exact direct
   * buffer capacity, so callers can safely reuse the buffer after inference returns.
   */
  public static void convertToRgb(
      int width, int height, Plane y, Plane u, Plane v, ByteBuffer destination) {
    if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0) {
      throw new IllegalArgumentException("YUV420 dimensions must be positive and even");
    }
    requireAddressable(y, width, height, "Y");
    requireAddressable(u, width / 2, height / 2, "U");
    requireAddressable(v, width / 2, height / 2, "V");
    Objects.requireNonNull(destination, "destination");
    if (!destination.isDirect()) {
      throw new IllegalArgumentException("RGB destination must be a direct buffer");
    }
    int rowStride = rgbRowStride(width);
    int requiredBytes = Math.multiplyExact(rowStride, height);
    if (destination.capacity() != requiredBytes) {
      throw new IllegalArgumentException(
          "RGB destination capacity must equal the aligned image size");
    }

    ByteBuffer output = destination.duplicate();
    output.clear();
    for (int row = 0; row < height; ++row) {
      int yInput = y.offset() + row * y.rowStride();
      int uInput = u.offset() + (row / 2) * u.rowStride();
      int vInput = v.offset() + (row / 2) * v.rowStride();
      int outputOffset = row * rowStride;
      for (int column = 0; column < width; ++column) {
        int luminance = (y.buffer().get(yInput + column * y.pixelStride()) & 0xff) - 16;
        int blueDifference =
            (u.buffer().get(uInput + (column / 2) * u.pixelStride()) & 0xff) - 128;
        int redDifference =
            (v.buffer().get(vInput + (column / 2) * v.pixelStride()) & 0xff) - 128;
        int scaledY = 298 * Math.max(0, luminance);
        int red = clamp((scaledY + 409 * redDifference + 128) >> 8);
        int green =
            clamp((scaledY - 100 * blueDifference - 208 * redDifference + 128) >> 8);
        int blue = clamp((scaledY + 516 * blueDifference + 128) >> 8);
        output.put(outputOffset, (byte) red);
        output.put(outputOffset + 1, (byte) green);
        output.put(outputOffset + 2, (byte) blue);
        outputOffset += 3;
      }
      int rowEnd = (row + 1) * rowStride;
      while (outputOffset < rowEnd) {
        output.put(outputOffset++, (byte) 0);
      }
    }
    destination.position(0);
    destination.limit(requiredBytes);
  }

  /** Exact buffer capacity required by MediaPipe's aligned three-channel JNI input. */
  public static int rgbBufferSize(int width, int height) {
    if (height <= 0) {
      throw new IllegalArgumentException("RGB height must be positive");
    }
    return Math.multiplyExact(rgbRowStride(width), height);
  }

  private static int rgbRowStride(int width) {
    if (width <= 0) {
      throw new IllegalArgumentException("RGB width must be positive");
    }
    int packedBytes = Math.multiplyExact(width, 3);
    return Math.multiplyExact(Math.floorDiv(Math.addExact(packedBytes, 3), 4), 4);
  }

  private static void requireAddressable(Plane plane, int columns, int rows, String name) {
    Objects.requireNonNull(plane, name);
    long last =
        (long) plane.offset()
            + (long) (rows - 1) * plane.rowStride()
            + (long) (columns - 1) * plane.pixelStride();
    if (last < 0 || last >= plane.buffer().limit()) {
      throw new IllegalArgumentException(name + " plane strides exceed its buffer");
    }
  }

  private static int clamp(int value) {
    return Math.min(255, Math.max(0, value));
  }
}
