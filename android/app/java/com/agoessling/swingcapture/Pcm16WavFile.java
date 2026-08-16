package com.agoessling.swingcapture;

import com.agoessling.swingcapture.audio.Pcm16EvidenceRing;
import com.agoessling.swingcapture.audio.Pcm16EvidenceRing.Snapshot;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Objects;
import java.util.Set;

/** Canonical mono 48 kHz PCM16LE WAV encoder and strict evidence metadata. */
public final class Pcm16WavFile {
  public static final String FILE_NAME = "audio_evidence.wav";
  public static final int HEADER_BYTES = 44;
  public static final Set<String> MANIFEST_FIELDS =
      Set.of(
          "path",
          "bytes",
          "sample_rate_hz",
          "first_frame_position",
          "last_frame_position",
          "strike_frame_position",
          "sample_count",
          "strike_sample_index");
  private static final int CHANNEL_COUNT = 1;
  private static final int BITS_PER_SAMPLE = 16;
  private static final int BYTES_PER_SAMPLE = 2;

  /** Exact manifest contract for one closed PCM evidence window. */
  public record EvidenceMetadata(
      String relativePath,
      long bytes,
      int sampleRateHz,
      long firstFramePosition,
      long lastFramePosition,
      long strikeFramePosition,
      int sampleCount,
      int strikeSampleIndex) {
    public EvidenceMetadata {
      Objects.requireNonNull(relativePath, "relativePath");
      if (!relativePath.equals(FILE_NAME)) {
        throw new IllegalArgumentException("audio evidence path must be " + FILE_NAME);
      }
      if (sampleRateHz != Pcm16EvidenceRing.SAMPLE_RATE_HZ
          || sampleCount != Pcm16EvidenceRing.SNAPSHOT_FRAME_COUNT
          || strikeSampleIndex != Pcm16EvidenceRing.PRE_ROLL_FRAMES) {
        throw new IllegalArgumentException("audio evidence PCM format or window is invalid");
      }
      if (firstFramePosition < 0
          || lastFramePosition < firstFramePosition
          || strikeFramePosition < firstFramePosition
          || strikeFramePosition > lastFramePosition) {
        throw new IllegalArgumentException("audio evidence absolute positions are invalid");
      }
      if (Math.addExact(firstFramePosition, sampleCount - 1L) != lastFramePosition
          || Math.addExact(firstFramePosition, strikeSampleIndex) != strikeFramePosition) {
        throw new IllegalArgumentException("audio evidence positions disagree with sample indexes");
      }
      if (bytes != expectedFileBytes(sampleCount)) {
        throw new IllegalArgumentException("audio evidence byte count is invalid");
      }
    }
  }

  private Pcm16WavFile() {}

  public static EvidenceMetadata metadata(Snapshot snapshot) {
    Objects.requireNonNull(snapshot, "snapshot");
    return new EvidenceMetadata(
        FILE_NAME,
        expectedFileBytes(snapshot.sampleCount()),
        Pcm16EvidenceRing.SAMPLE_RATE_HZ,
        snapshot.firstFramePosition(),
        snapshot.lastFramePosition(),
        snapshot.strikeFramePosition(),
        snapshot.sampleCount(),
        snapshot.strikeSampleIndex());
  }

  /** Writes exactly one RIFF/WAVE PCM file without closing the caller-owned stream. */
  public static void write(Snapshot snapshot, OutputStream output) throws IOException {
    Objects.requireNonNull(snapshot, "snapshot");
    Objects.requireNonNull(output, "output");
    int dataBytes = Math.multiplyExact(snapshot.sampleCount(), BYTES_PER_SAMPLE);
    writeAscii(output, "RIFF");
    writeUnsigned32Le(output, 36L + dataBytes);
    writeAscii(output, "WAVE");
    writeAscii(output, "fmt ");
    writeUnsigned32Le(output, 16);
    writeUnsigned16Le(output, 1);
    writeUnsigned16Le(output, CHANNEL_COUNT);
    writeUnsigned32Le(output, Pcm16EvidenceRing.SAMPLE_RATE_HZ);
    writeUnsigned32Le(
        output, (long) Pcm16EvidenceRing.SAMPLE_RATE_HZ * CHANNEL_COUNT * BYTES_PER_SAMPLE);
    writeUnsigned16Le(output, CHANNEL_COUNT * BYTES_PER_SAMPLE);
    writeUnsigned16Le(output, BITS_PER_SAMPLE);
    writeAscii(output, "data");
    writeUnsigned32Le(output, dataBytes);
    for (int index = 0; index < snapshot.sampleCount(); ++index) {
      int sample = snapshot.sampleAt(index) & 0xffff;
      output.write(sample & 0xff);
      output.write((sample >>> 8) & 0xff);
    }
  }

  public static long expectedFileBytes(int sampleCount) {
    if (sampleCount < 0) {
      throw new IllegalArgumentException("sample count must be nonnegative");
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
