package com.agoessling.swingcapture;

import java.util.List;

/** Hermetic schema, canonical encoding, and timing-consistency tests. */
public final class FieldRecordingManifestTest {
  private FieldRecordingManifestTest() {}

  public static void main(String[] arguments) {
    emitsCanonicalFieldRecordingSchema();
    rejectsContradictoryTiming();
  }

  private static void emitsCanonicalFieldRecordingSchema() {
    FieldRecordingManifest.Data data = validData();
    String first = FieldRecordingManifest.toCanonicalJson(data);
    String second = FieldRecordingManifest.toCanonicalJson(data);
    check(first.equals(second), "deterministic serialization");
    check(first.endsWith("\n"), "one trailing newline");
    check(
        first.startsWith(
            "{\"schema_version\":1,\"session_kind\":\"field_recording\","
                + "\"recording_id\":\"field-node-a\","
                + "\"shared_recording_id\":\"shared-1\""),
        "stable root schema order");
    check(first.contains("\"node_id\":\"node-a\",\"role\":\"face_on\""), "identity");
    check(first.contains("\"duration_us\":\"2000000\""), "exact duration");
    check(
        first.contains(
            "\"video\":{\"path\":\"video.mp4\",\"mime_type\":\"video/mp4\","
                + "\"width\":1280,\"height\":720,\"nominal_fps\":30"),
        "video schema");
    check(
        first.contains(
            "\"audio\":{\"path\":\"audio.wav\",\"mime_type\":\"audio/wav\","
                + "\"sample_rate_hz\":48000,\"channel_count\":1,"
                + "\"encoding\":\"pcm_s16le\""),
        "audio schema");
    check(first.contains("\"clock\":\"CLOCK_BOOTTIME\""), "monotonic clock domain");
    check(
        first.contains(
            "\"encoder_pts_clock\":\"CLOCK_MONOTONIC\","
                + "\"camera_timestamp_source\":1,"
                + "\"clock_anchor\":{"
                + "\"source_clock\":\"CLOCK_MONOTONIC\","
                + "\"target_clock\":\"CLOCK_BOOTTIME\","
                + "\"boottime_minus_monotonic_ns\":\"200000000\","
                + "\"monotonic_before_ns\":\"900000000\","
                + "\"boottime_ns\":\"1100000005\","
                + "\"monotonic_after_ns\":\"900000010\","
                + "\"bracket_ns\":\"10\",\"uncertainty_ns\":\"5\"}"),
        "explicit encoder-to-boottime clock anchor");
    check(
        first.contains(
            "\"audio_record_frame_position\":\"4800\","
                + "\"boottime_ns\":\"1100000000\""),
        "audio mapping evidence");
  }

  private static void rejectsContradictoryTiming() {
    FieldRecordingManifest.Data valid = validData();
    expectInvalid(
        () ->
            new FieldRecordingManifest.Data(
                valid.recordingId(),
                valid.sharedRecordingId(),
                valid.nodeId(),
                valid.role(),
                valid.createdAtUtc(),
                valid.startedElapsedRealtimeNs(),
                valid.stoppedElapsedRealtimeNs(),
                valid.orientationDegrees(),
                valid.stopReason(),
                valid.videoCodec(),
                valid.videoBytes(),
                valid.firstVideoPtsUs(),
                valid.lastVideoPtsUs(),
                valid.audioSource(),
                valid.audioFrames(),
                valid.audioBytes() - 1,
                valid.cameraTimestampSource(),
                valid.clockAnchor(),
                valid.cameraFrames(),
                valid.encodedSamples(),
                valid.audioTimestamps()),
        "WAV byte mismatch");
    expectInvalid(
        () ->
            new FieldRecordingManifest.Data(
                valid.recordingId(),
                valid.sharedRecordingId(),
                valid.nodeId(),
                valid.role(),
                valid.createdAtUtc(),
                valid.startedElapsedRealtimeNs(),
                valid.stoppedElapsedRealtimeNs(),
                valid.orientationDegrees(),
                valid.stopReason(),
                valid.videoCodec(),
                valid.videoBytes(),
                valid.firstVideoPtsUs(),
                valid.lastVideoPtsUs(),
                valid.audioSource(),
                valid.audioFrames(),
                valid.audioBytes(),
                valid.cameraTimestampSource(),
                valid.clockAnchor(),
                List.of(
                    new FieldRecordingManifest.CameraFrame(2, 1_200_000_000L),
                    new FieldRecordingManifest.CameraFrame(1, 1_300_000_000L)),
                valid.encodedSamples(),
                valid.audioTimestamps()),
        "nonmonotonic camera evidence");
    expectInvalid(
        () -> new FieldRecordingManifest.ClockAnchor(10, 20, 9),
        "reversed clock anchor bracket");
    FieldRecordingManifest.ClockAnchor oddBracket =
        new FieldRecordingManifest.ClockAnchor(100, 1_103, 105);
    check(oddBracket.boottimeMinusMonotonicNs() == 1_001, "midpoint offset rounds down");
    check(oddBracket.bracketNs() == 5, "raw bracket width");
    check(oddBracket.uncertaintyNs() == 3, "uncertainty rounds up");
  }

  private static FieldRecordingManifest.Data validData() {
    long audioFrames = 96_000;
    return new FieldRecordingManifest.Data(
        "field-node-a",
        "shared-1",
        "node-a",
        "face_on",
        "2026-08-22T18:00:00Z",
        1_000_000_000L,
        3_000_000_000L,
        90,
        "explicit",
        "avc1.64001f",
        123_456,
        1_000_000,
        1_033_333,
        9,
        audioFrames,
        StreamingPcm16WavFile.expectedFileBytes(audioFrames),
        1,
        new FieldRecordingManifest.ClockAnchor(
            900_000_000L, 1_100_000_005L, 900_000_010L),
        List.of(
            new FieldRecordingManifest.CameraFrame(10, 1_010_000_000L),
            new FieldRecordingManifest.CameraFrame(11, 1_043_333_333L)),
        List.of(
            new FieldRecordingManifest.EncodedSample(0, 1_000_000, 0, 1, 1000),
            new FieldRecordingManifest.EncodedSample(1, 1_033_333, 33_333, 0, 900)),
        List.of(
            new FieldRecordingManifest.AudioTimestampObservation(
                4_800, 4_800, 1_100_000_000L, 250_000),
            new FieldRecordingManifest.AudioTimestampObservation(
                9_600, 9_600, 1_200_000_000L, 250_000)));
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
