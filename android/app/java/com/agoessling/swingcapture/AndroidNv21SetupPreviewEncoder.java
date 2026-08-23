package com.agoessling.swingcapture;

import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.graphics.YuvImage;
import java.io.OutputStream;

/** Android JPEG encoder with a fixed-capacity output buffer for setup previews. */
final class AndroidNv21SetupPreviewEncoder implements SetupPreviewProvider.Encoder {
  @Override
  public byte[] encodeNv21(
      byte[] nv21,
      int width,
      int height,
      int jpegQuality,
      int maximumJpegBytes) {
    FixedCapacityOutput output = new FixedCapacityOutput(maximumJpegBytes);
    YuvImage image = new YuvImage(nv21, ImageFormat.NV21, width, height, null);
    if (!image.compressToJpeg(new Rect(0, 0, width, height), jpegQuality, output)) {
      throw new IllegalStateException("setup preview JPEG encoding failed");
    }
    return output.toByteArray();
  }

  private static final class FixedCapacityOutput extends OutputStream {
    private final byte[] bytes;
    private int size;

    private FixedCapacityOutput(int capacity) {
      if (capacity <= 0) {
        throw new IllegalArgumentException("setup preview JPEG capacity must be positive");
      }
      bytes = new byte[capacity];
    }

    @Override
    public void write(int value) {
      requireCapacity(1);
      bytes[size++] = (byte) value;
    }

    @Override
    public void write(byte[] source, int offset, int length) {
      if (source == null
          || offset < 0
          || length < 0
          || offset > source.length - length) {
        throw new IndexOutOfBoundsException("invalid setup preview JPEG write range");
      }
      requireCapacity(length);
      System.arraycopy(source, offset, bytes, size, length);
      size += length;
    }

    private void requireCapacity(int additionalBytes) {
      if (additionalBytes > bytes.length - size) {
        throw new IllegalStateException("setup preview JPEG exceeded its byte bound");
      }
    }

    private byte[] toByteArray() {
      return java.util.Arrays.copyOf(bytes, size);
    }
  }
}
