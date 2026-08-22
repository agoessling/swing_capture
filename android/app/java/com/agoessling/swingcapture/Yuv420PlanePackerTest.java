package com.agoessling.swingcapture;

import java.nio.ByteBuffer;
import java.util.Arrays;

public final class Yuv420PlanePackerTest {
  public static void main(String[] args) {
    packsPaddedAndInterleavedStrides();
    honorsOffsetsAndUnitPixelStrides();
    rejectsTruncatedPlanes();
    convertsLimitedRangeNeutralPixelsToArgb();
  }

  private static void convertsLimitedRangeNeutralPixelsToArgb() {
    int[] pixels =
        Yuv420PlanePacker.toArgb(
            2,
            2,
            plane(new byte[] {16, (byte) 235, 81, (byte) 145}, 0, 2, 1),
            plane(new byte[] {(byte) 128}, 0, 1, 1),
            plane(new byte[] {(byte) 128}, 0, 1, 1));
    check(pixels[0] == 0xff000000, "limited black");
    check(pixels[1] == 0xffffffff, "limited white");
    check(pixels[2] == 0xff4c4c4c, "neutral dark gray");
    check(pixels[3] == 0xff969696, "neutral light gray");
  }

  private static void packsPaddedAndInterleavedStrides() {
    byte[] y = {1, 2, 3, 4, 99, 99, 5, 6, 7, 8};
    byte[] u = {11, 99, 12};
    byte[] v = {21, 99, 22};
    byte[] actual =
        Yuv420PlanePacker.toNv21(
            4,
            2,
            plane(y, 0, 6, 1),
            plane(u, 0, 4, 2),
            plane(v, 0, 4, 2));
    check(
        Arrays.equals(actual, new byte[] {1, 2, 3, 4, 5, 6, 7, 8, 21, 11, 22, 12}),
        "padded/interleaved plane packing mismatch");
  }

  private static void honorsOffsetsAndUnitPixelStrides() {
    byte[] actual =
        Yuv420PlanePacker.toNv21(
            2,
            2,
            plane(new byte[] {99, 1, 2, 3, 4}, 1, 2, 1),
            plane(new byte[] {99, 11}, 1, 1, 1),
            plane(new byte[] {99, 21}, 1, 1, 1));
    check(
        Arrays.equals(actual, new byte[] {1, 2, 3, 4, 21, 11}),
        "offset plane packing mismatch");
  }

  private static void rejectsTruncatedPlanes() {
    expectIllegalArgument(
        () ->
            Yuv420PlanePacker.toNv21(
                4,
                2,
                plane(new byte[7], 0, 4, 1),
                plane(new byte[2], 0, 2, 1),
                plane(new byte[2], 0, 2, 1)));
  }

  private static Yuv420PlanePacker.Plane plane(
      byte[] bytes, int offset, int rowStride, int pixelStride) {
    return new Yuv420PlanePacker.Plane(
        ByteBuffer.wrap(bytes), offset, rowStride, pixelStride);
  }

  private static void expectIllegalArgument(Runnable action) {
    try {
      action.run();
      throw new AssertionError("expected IllegalArgumentException");
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
