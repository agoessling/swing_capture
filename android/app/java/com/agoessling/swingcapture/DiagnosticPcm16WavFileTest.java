package com.agoessling.swingcapture;

import com.agoessling.swingcapture.diagnostics.DiagnosticAudioRing;
import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/** Variable-window RIFF/WAVE and marker metadata coverage. */
public final class DiagnosticPcm16WavFileTest {
  private DiagnosticPcm16WavFileTest() {}

  public static void main(String[] arguments) throws Exception {
    writesCanonicalVariableWindow();
    rejectsMarkerOutsideWindow();
  }

  private static void writesCanonicalVariableWindow() throws Exception {
    DiagnosticAudioRing ring = new DiagnosticAudioRing(16);
    short[] samples = {(short) 0x1234, (short) 0x80ff, 0, 1, 2, (short) 0xabcd};
    ring.append(samples, 0, samples.length, 100);
    DiagnosticAudioRing.Snapshot snapshot = ring.snapshot(101, 106);
    DiagnosticPcm16WavFile.EvidenceMetadata metadata =
        DiagnosticPcm16WavFile.metadata(snapshot, 104);
    ByteArrayOutputStream output = new ByteArrayOutputStream();
    DiagnosticPcm16WavFile.write(snapshot, output);
    byte[] wav = output.toByteArray();

    check(ascii(wav, 0, 4).equals("RIFF"), "RIFF");
    check(unsigned32(wav, 4) == wav.length - 8L, "RIFF bytes");
    check(ascii(wav, 8, 4).equals("WAVE"), "WAVE");
    check(unsigned32(wav, 24) == 48_000, "sample rate");
    check(unsigned32(wav, 40) == 10, "payload bytes");
    check((wav[44] & 0xff) == 0xff && (wav[45] & 0xff) == 0x80, "first sample");
    check((wav[wav.length - 2] & 0xff) == 0xcd, "last sample");
    check(metadata.bytes() == wav.length, "metadata bytes");
    check(metadata.firstFramePosition() == 101, "first frame");
    check(metadata.endFramePosition() == 106, "exclusive end frame");
    check(metadata.markerFramePosition() == 104, "marker frame");
    check(metadata.markerSampleIndex() == 3, "marker index");
  }

  private static void rejectsMarkerOutsideWindow() {
    DiagnosticAudioRing ring = new DiagnosticAudioRing(4);
    ring.append(new short[] {1, 2, 3, 4}, 0, 4, 10);
    DiagnosticAudioRing.Snapshot snapshot = ring.snapshot(10, 14);
    try {
      DiagnosticPcm16WavFile.metadata(snapshot, 14);
      throw new AssertionError("Expected marker rejection");
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
  }

  private static String ascii(byte[] value, int offset, int count) {
    return new String(
        Arrays.copyOfRange(value, offset, offset + count), StandardCharsets.US_ASCII);
  }

  private static long unsigned32(byte[] value, int offset) {
    return (value[offset] & 0xffL)
        | ((value[offset + 1] & 0xffL) << 8)
        | ((value[offset + 2] & 0xffL) << 16)
        | ((value[offset + 3] & 0xffL) << 24);
  }

  private static void check(boolean condition, String label) {
    if (!condition) {
      throw new AssertionError(label);
    }
  }
}
