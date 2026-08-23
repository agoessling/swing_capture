package com.agoessling.swingcapture;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.Objects;

/** Streaming canonical mono 48 kHz PCM16LE WAV writer with a patched, synced final header. */
final class StreamingPcm16WavFile implements AutoCloseable {
  static final int HEADER_BYTES = 44;
  private static final int BYTES_PER_FRAME = 2;

  private final RandomAccessFile output;
  private long frames;
  private boolean closed;

  StreamingPcm16WavFile(File target) throws IOException {
    Objects.requireNonNull(target, "target");
    if (target.exists()) {
      throw new IOException("WAV staging file already exists");
    }
    output = new RandomAccessFile(target, "rw");
    writeHeader(0);
  }

  synchronized void append(short[] samples, int offset, int count) throws IOException {
    Objects.checkFromIndexSize(offset, count, samples.length);
    if (closed) {
      throw new IllegalStateException("WAV writer is closed");
    }
    long nextFrames = Math.addExact(frames, count);
    expectedFileBytes(nextFrames);
    byte[] bytes = new byte[Math.multiplyExact(count, BYTES_PER_FRAME)];
    for (int index = 0; index < count; ++index) {
      int sample = samples[offset + index] & 0xffff;
      bytes[index * 2] = (byte) sample;
      bytes[index * 2 + 1] = (byte) (sample >>> 8);
    }
    output.write(bytes);
    frames = nextFrames;
  }

  synchronized long frames() {
    return frames;
  }

  synchronized long bytes() {
    return expectedFileBytes(frames);
  }

  @Override
  public synchronized void close() throws IOException {
    if (closed) {
      return;
    }
    IOException failure = null;
    try {
      output.seek(0);
      writeHeader(frames);
      output.getFD().sync();
    } catch (IOException closeFailure) {
      failure = closeFailure;
    }
    try {
      output.close();
    } catch (IOException closeFailure) {
      if (failure == null) {
        failure = closeFailure;
      } else {
        failure.addSuppressed(closeFailure);
      }
    }
    closed = true;
    if (failure != null) {
      throw failure;
    }
  }

  static long expectedFileBytes(long frames) {
    if (frames < 0 || frames > (0xffff_ffffL - 36L) / BYTES_PER_FRAME) {
      throw new IllegalArgumentException("PCM frame count exceeds canonical WAV bounds");
    }
    return Math.addExact(HEADER_BYTES, Math.multiplyExact(frames, BYTES_PER_FRAME));
  }

  private void writeHeader(long frameCount) throws IOException {
    long dataBytes = Math.multiplyExact(frameCount, BYTES_PER_FRAME);
    writeAscii("RIFF");
    writeUnsigned32Le(36 + dataBytes);
    writeAscii("WAVE");
    writeAscii("fmt ");
    writeUnsigned32Le(16);
    writeUnsigned16Le(1);
    writeUnsigned16Le(1);
    writeUnsigned32Le(FieldRecordingManifest.AUDIO_SAMPLE_RATE_HZ);
    writeUnsigned32Le((long) FieldRecordingManifest.AUDIO_SAMPLE_RATE_HZ * BYTES_PER_FRAME);
    writeUnsigned16Le(BYTES_PER_FRAME);
    writeUnsigned16Le(16);
    writeAscii("data");
    writeUnsigned32Le(dataBytes);
  }

  private void writeAscii(String value) throws IOException {
    output.writeBytes(value);
  }

  private void writeUnsigned16Le(int value) throws IOException {
    output.write(value & 0xff);
    output.write((value >>> 8) & 0xff);
  }

  private void writeUnsigned32Le(long value) throws IOException {
    if (value < 0 || value > 0xffff_ffffL) {
      throw new IllegalArgumentException("value exceeds unsigned 32-bit range");
    }
    output.write((int) value & 0xff);
    output.write((int) (value >>> 8) & 0xff);
    output.write((int) (value >>> 16) & 0xff);
    output.write((int) (value >>> 24) & 0xff);
  }
}
