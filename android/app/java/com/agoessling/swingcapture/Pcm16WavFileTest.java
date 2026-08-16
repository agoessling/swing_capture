package com.agoessling.swingcapture;

import com.agoessling.swingcapture.audio.Pcm16EvidenceRing;
import com.agoessling.swingcapture.audio.Pcm16EvidenceRing.Snapshot;
import java.io.ByteArrayOutputStream;
import java.util.Arrays;

/** Pure canonical WAV, byte-order, metadata, and HIL-only allocation policy tests. */
public final class Pcm16WavFileTest {
  private Pcm16WavFileTest() {}

  public static void main(String[] arguments) throws Exception {
    canonicalHeaderAndSamplesAreLittleEndian();
    metadataRejectsContradictoryOrUnsafeValues();
    ringAllocationAndPublicationAreAudioHilOnly();
  }

  private static void canonicalHeaderAndSamplesAreLittleEndian() throws Exception {
    Pcm16EvidenceRing ring =
        new Pcm16EvidenceRing(AudioEvidencePolicy.RETENTION_FRAMES);
    short[] samples = new short[Pcm16EvidenceRing.SNAPSHOT_FRAME_COUNT];
    samples[0] = (short) 0x1234;
    samples[1] = (short) 0x80ff;
    samples[samples.length - 1] = (short) 0xabcd;
    ring.append(samples, 0, samples.length, 100_000);
    long strike = 100_000L + Pcm16EvidenceRing.PRE_ROLL_FRAMES;
    Snapshot snapshot = ring.snapshot(strike);

    ByteArrayOutputStream output = new ByteArrayOutputStream();
    Pcm16WavFile.write(snapshot, output);
    byte[] wav = output.toByteArray();
    Pcm16WavFile.EvidenceMetadata metadata = Pcm16WavFile.metadata(snapshot);

    check(wav.length == metadata.bytes(), "exact file byte count");
    check(ascii(wav, 0, 4).equals("RIFF"), "RIFF marker");
    check(unsigned32(wav, 4) == wav.length - 8L, "RIFF chunk length");
    check(ascii(wav, 8, 4).equals("WAVE"), "WAVE marker");
    check(ascii(wav, 12, 4).equals("fmt "), "fmt marker");
    check(unsigned32(wav, 16) == 16, "canonical fmt size");
    check(unsigned16(wav, 20) == 1, "PCM format");
    check(unsigned16(wav, 22) == 1, "mono channel count");
    check(unsigned32(wav, 24) == 48_000, "sample rate");
    check(unsigned32(wav, 28) == 96_000, "byte rate");
    check(unsigned16(wav, 32) == 2, "block alignment");
    check(unsigned16(wav, 34) == 16, "bits per sample");
    check(ascii(wav, 36, 4).equals("data"), "data marker");
    check(unsigned32(wav, 40) == samples.length * 2L, "PCM payload length");
    check((wav[44] & 0xff) == 0x34 && (wav[45] & 0xff) == 0x12, "positive LE sample");
    check((wav[46] & 0xff) == 0xff && (wav[47] & 0xff) == 0x80, "negative LE sample");
    check(
        (wav[wav.length - 2] & 0xff) == 0xcd && (wav[wav.length - 1] & 0xff) == 0xab,
        "final LE sample");
    check(metadata.relativePath().equals("audio_evidence.wav"), "relative path");
    check(metadata.firstFramePosition() == 100_000, "absolute first frame");
    check(metadata.lastFramePosition() == 196_000, "absolute last frame");
    check(metadata.strikeFramePosition() == strike, "absolute strike frame");
    check(metadata.strikeSampleIndex() == 72_000, "strike sample index");
  }

  private static void metadataRejectsContradictoryOrUnsafeValues() {
    long bytes = Pcm16WavFile.expectedFileBytes(Pcm16EvidenceRing.SNAPSHOT_FRAME_COUNT);
    expectInvalid(
        () ->
            new Pcm16WavFile.EvidenceMetadata(
                "../audio_evidence.wav",
                bytes,
                48_000,
                10,
                96_010,
                72_010,
                96_001,
                72_000),
        "unsafe path");
    expectInvalid(
        () ->
            new Pcm16WavFile.EvidenceMetadata(
                Pcm16WavFile.FILE_NAME,
                bytes - 1,
                48_000,
                10,
                96_010,
                72_010,
                96_001,
                72_000),
        "wrong bytes");
    expectInvalid(
        () ->
            new Pcm16WavFile.EvidenceMetadata(
                Pcm16WavFile.FILE_NAME,
                bytes,
                48_000,
                10,
                96_009,
                72_010,
                96_001,
                72_000),
        "wrong last position");
  }

  private static void ringAllocationAndPublicationAreAudioHilOnly() {
    check(AudioEvidencePolicy.ringForCapture(false) == null, "production allocates no PCM ring");
    Pcm16EvidenceRing hilRing = AudioEvidencePolicy.ringForCapture(true);
    check(hilRing != null, "audio HIL allocates PCM ring");
    check(
        hilRing.capacityFrames() == 4 * Pcm16EvidenceRing.SAMPLE_RATE_HZ,
        "audio HIL retains four seconds");
    check(
        AudioEvidencePolicy.requiresPublishedEvidence(hilRing, "local_audio"),
        "audio HIL local trigger requires WAV");
    check(
        !AudioEvidencePolicy.requiresPublishedEvidence(hilRing, "manual"),
        "manual trigger publishes no WAV");
    check(
        !AudioEvidencePolicy.requiresPublishedEvidence(null, "local_audio"),
        "ordinary local trigger publishes no WAV");
  }

  private static String ascii(byte[] value, int offset, int count) {
    return new String(Arrays.copyOfRange(value, offset, offset + count), java.nio.charset.StandardCharsets.US_ASCII);
  }

  private static int unsigned16(byte[] value, int offset) {
    return (value[offset] & 0xff) | ((value[offset + 1] & 0xff) << 8);
  }

  private static long unsigned32(byte[] value, int offset) {
    return (value[offset] & 0xffL)
        | ((value[offset + 1] & 0xffL) << 8)
        | ((value[offset + 2] & 0xffL) << 16)
        | ((value[offset + 3] & 0xffL) << 24);
  }

  private static void expectInvalid(Action action, String label) {
    try {
      action.run();
      throw new AssertionError("Expected invalid metadata: " + label);
    } catch (IllegalArgumentException expected) {
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
    void run();
  }
}
