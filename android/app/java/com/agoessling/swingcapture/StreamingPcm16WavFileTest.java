package com.agoessling.swingcapture;

import java.io.File;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;

/** Hermetic streaming WAV finalization and bounds tests. */
public final class StreamingPcm16WavFileTest {
  private StreamingPcm16WavFileTest() {}

  public static void main(String[] arguments) throws Exception {
    streamsAndPatchesCanonicalHeader();
    rejectsUnsafeLifecycleAndBounds();
  }

  private static void streamsAndPatchesCanonicalHeader() throws Exception {
    File directory = Files.createTempDirectory("field-wav-test").toFile();
    File output = new File(directory, "audio.wav.tmp");
    try {
      StreamingPcm16WavFile writer = new StreamingPcm16WavFile(output);
      writer.append(new short[] {(short) 0x1234, (short) 0x80ff}, 0, 2);
      writer.append(new short[] {0, (short) 0xabcd, 0}, 1, 1);
      check(writer.frames() == 3, "streamed frame count");
      writer.close();
      writer.close();

      byte[] wav = Files.readAllBytes(output.toPath());
      check(wav.length == 50, "final byte count");
      check(ascii(wav, 0, 4).equals("RIFF"), "RIFF marker");
      check(unsigned32(wav, 4) == 42, "patched RIFF length");
      check(ascii(wav, 8, 4).equals("WAVE"), "WAVE marker");
      check(unsigned32(wav, 24) == 48_000, "sample rate");
      check(unsigned32(wav, 28) == 96_000, "byte rate");
      check(unsigned32(wav, 40) == 6, "patched payload length");
      check((wav[44] & 0xff) == 0x34 && (wav[45] & 0xff) == 0x12, "sample one");
      check((wav[46] & 0xff) == 0xff && (wav[47] & 0xff) == 0x80, "sample two");
      check((wav[48] & 0xff) == 0xcd && (wav[49] & 0xff) == 0xab, "sample three");
    } finally {
      Files.deleteIfExists(output.toPath());
      Files.deleteIfExists(directory.toPath());
    }
  }

  private static void rejectsUnsafeLifecycleAndBounds() throws Exception {
    File directory = Files.createTempDirectory("field-wav-lifecycle-test").toFile();
    File output = new File(directory, "audio.wav.tmp");
    try {
      StreamingPcm16WavFile writer = new StreamingPcm16WavFile(output);
      writer.close();
      expectInvalidState(() -> writer.append(new short[1], 0, 1), "append after close");
      expectInvalidArgument(
          () -> StreamingPcm16WavFile.expectedFileBytes(-1), "negative frame count");
      expectIo(() -> new StreamingPcm16WavFile(output), "existing staging file");
    } finally {
      Files.deleteIfExists(output.toPath());
      Files.deleteIfExists(directory.toPath());
    }
  }

  private static String ascii(byte[] bytes, int offset, int count) {
    return new String(
        Arrays.copyOfRange(bytes, offset, offset + count), StandardCharsets.US_ASCII);
  }

  private static long unsigned32(byte[] bytes, int offset) {
    return (bytes[offset] & 0xffL)
        | ((bytes[offset + 1] & 0xffL) << 8)
        | ((bytes[offset + 2] & 0xffL) << 16)
        | ((bytes[offset + 3] & 0xffL) << 24);
  }

  private static void expectInvalidState(Action action, String label) throws Exception {
    try {
      action.run();
      throw new AssertionError("Expected invalid state: " + label);
    } catch (IllegalStateException expected) {
      // Expected.
    }
  }

  private static void expectInvalidArgument(Action action, String label) throws Exception {
    try {
      action.run();
      throw new AssertionError("Expected invalid argument: " + label);
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
  }

  private static void expectIo(Action action, String label) throws Exception {
    try {
      action.run();
      throw new AssertionError("Expected I/O failure: " + label);
    } catch (java.io.IOException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String label) {
    if (!condition) {
      throw new AssertionError(label);
    }
  }

  @FunctionalInterface
  private interface Action {
    void run() throws Exception;
  }
}
