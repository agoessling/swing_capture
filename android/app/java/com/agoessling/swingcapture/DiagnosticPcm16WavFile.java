package com.agoessling.swingcapture;

import com.agoessling.swingcapture.diagnostics.DiagnosticAudioRing;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Objects;

/** Canonical mono PCM16LE WAV writer for a variable flight-recorder evidence window. */
public final class DiagnosticPcm16WavFile {
  public static final String FILE_NAME = "diagnostic_audio.wav";
  public static final int SAMPLE_RATE_HZ = 48_000;
  public static final int HEADER_BYTES = 44;
  private static final int BYTES_PER_SAMPLE = 2;

  public record EvidenceMetadata(
      String relativePath,
      long bytes,
      int sampleRateHz,
      long firstFramePosition,
      long endFramePosition,
      long markerFramePosition,
      int sampleCount,
      int markerSampleIndex) {
    public EvidenceMetadata {
      Objects.requireNonNull(relativePath, "relativePath");
      if (!relativePath.equals(FILE_NAME)
          || sampleRateHz != SAMPLE_RATE_HZ
          || firstFramePosition < 0
          || endFramePosition <= firstFramePosition
          || markerFramePosition < firstFramePosition
          || markerFramePosition >= endFramePosition
          || sampleCount != endFramePosition - firstFramePosition
          || markerSampleIndex != markerFramePosition - firstFramePosition
          || bytes != expectedFileBytes(sampleCount)) {
        throw new IllegalArgumentException("diagnostic audio evidence metadata is invalid");
      }
    }
  }

  private DiagnosticPcm16WavFile() {}

  public static EvidenceMetadata metadata(
      DiagnosticAudioRing.Snapshot snapshot, long markerFramePosition) {
    Objects.requireNonNull(snapshot, "snapshot");
    return new EvidenceMetadata(
        FILE_NAME,
        expectedFileBytes(snapshot.frameCount()),
        SAMPLE_RATE_HZ,
        snapshot.firstFramePosition(),
        snapshot.endFramePosition(),
        markerFramePosition,
        snapshot.frameCount(),
        Math.toIntExact(markerFramePosition - snapshot.firstFramePosition()));
  }

  public static void write(DiagnosticAudioRing.Snapshot snapshot, OutputStream output)
      throws IOException {
    Objects.requireNonNull(snapshot, "snapshot");
    Objects.requireNonNull(output, "output");
    int dataBytes = Math.multiplyExact(snapshot.frameCount(), BYTES_PER_SAMPLE);
    writeAscii(output, "RIFF");
    writeUnsigned32Le(output, 36L + dataBytes);
    writeAscii(output, "WAVE");
    writeAscii(output, "fmt ");
    writeUnsigned32Le(output, 16);
    writeUnsigned16Le(output, 1);
    writeUnsigned16Le(output, 1);
    writeUnsigned32Le(output, SAMPLE_RATE_HZ);
    writeUnsigned32Le(output, (long) SAMPLE_RATE_HZ * BYTES_PER_SAMPLE);
    writeUnsigned16Le(output, BYTES_PER_SAMPLE);
    writeUnsigned16Le(output, 16);
    writeAscii(output, "data");
    writeUnsigned32Le(output, dataBytes);
    for (int index = 0; index < snapshot.frameCount(); ++index) {
      int sample = snapshot.sampleAt(index) & 0xffff;
      output.write(sample & 0xff);
      output.write((sample >>> 8) & 0xff);
    }
  }

  public static long expectedFileBytes(int sampleCount) {
    if (sampleCount <= 0) {
      throw new IllegalArgumentException("diagnostic audio sample count must be positive");
    }
    return Math.addExact(HEADER_BYTES, Math.multiplyExact((long) sampleCount, BYTES_PER_SAMPLE));
  }

  private static void writeAscii(OutputStream output, String value) throws IOException {
    output.write(value.getBytes(StandardCharsets.US_ASCII));
  }

  private static void writeUnsigned16Le(OutputStream output, int value) throws IOException {
    if (value < 0 || value > 0xffff) {
      throw new IllegalArgumentException("value exceeds unsigned 16-bit range");
    }
    output.write(value & 0xff);
    output.write((value >>> 8) & 0xff);
  }

  private static void writeUnsigned32Le(OutputStream output, long value) throws IOException {
    if (value < 0 || value > 0xffff_ffffL) {
      throw new IllegalArgumentException("value exceeds unsigned 32-bit range");
    }
    output.write((int) value & 0xff);
    output.write((int) (value >>> 8) & 0xff);
    output.write((int) (value >>> 16) & 0xff);
    output.write((int) (value >>> 24) & 0xff);
  }
}
