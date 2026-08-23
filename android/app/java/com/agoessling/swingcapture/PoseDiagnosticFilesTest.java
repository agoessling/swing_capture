package com.agoessling.swingcapture;

import com.agoessling.swingcapture.diagnostics.PreviewEvidence;
import com.agoessling.swingcapture.diagnostics.PreviewEvidenceRing;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Deterministic host coverage for bounded atomic pose diagnostic publication. */
public final class PoseDiagnosticFilesTest {
  private PoseDiagnosticFilesTest() {}

  public static void main(String[] arguments) throws Exception {
    publishesCanonicalIndexedEvidence();
    publishesFullRecommendedCountAsTwoFiles();
    publishesFiveHzTraceWithLowerCadenceJpegs();
    rejectsEmptyOversizedAndMalformedSnapshots();
    rejectsUnsafeOrConflictingPaths();
    rejectsInconsistentManifestMetadata();
    failedDirectorySyncCleansStagingUnit();
  }

  private static void publishesCanonicalIndexedEvidence() throws Exception {
    File session = temporarySession("canonical");
    PreviewEvidenceRing ring = new PreviewEvidenceRing(1_000, 10, 1_000);
    byte[] first = jpeg(1, 2);
    byte[] second = jpeg(3);
    ring.append(
        evidence(
            100,
            first,
            "model\"alpha",
            7,
            0.75,
            0.5,
            0.0,
            true,
            PreviewEvidence.ControllerState.MONITORING,
            "wait\nnow"));
    ring.append(
        evidence(
            300,
            second,
            "model-beta",
            11,
            0.9,
            0.8,
            0.125,
            true,
            PreviewEvidence.ControllerState.HIGH_SPEED_REQUESTED,
            "stable"));
    List<String> synced = new ArrayList<>();

    PoseDiagnosticFiles.ManifestMetadata metadata =
        new PoseDiagnosticFiles(directory -> synced.add(directory.getName()))
            .publish(session, ring.snapshot());

    check(
        synced.equals(List.of("pose_diagnostics.tmp", "canonical.tmp")),
        "directory synchronization order");
    check(metadata.frameCount() == 2, "metadata frame count");
    check(metadata.observationCount() == 2, "metadata observation count");
    check(metadata.framesBytes() == first.length + second.length, "metadata frame bytes");
    check(metadata.traceBytes() > 0, "metadata trace bytes");
    check(metadata.firstTimestampInclusive() == 100, "metadata first timestamp");
    check(metadata.endTimestampExclusive() == 301, "metadata end timestamp");
    check(metadata.frames().get(0).byteOffset() == 0, "first frame offset");
    check(metadata.frames().get(1).byteOffset() == first.length, "second frame offset");

    File published = new File(session, PoseDiagnosticFiles.DIRECTORY_NAME);
    check(published.isDirectory(), "published evidence directory");
    check(!new File(session, PoseDiagnosticFiles.DIRECTORY_NAME + ".tmp").exists(), "no staging");
    byte[] expectedFrames = new byte[first.length + second.length];
    System.arraycopy(first, 0, expectedFrames, 0, first.length);
    System.arraycopy(second, 0, expectedFrames, first.length, second.length);
    check(
        Arrays.equals(
            Files.readAllBytes(new File(published, PoseDiagnosticFiles.FRAMES_FILE_NAME).toPath()),
            expectedFrames),
        "exact concatenated JPEG bytes");

    String expectedTrace =
        "{\"schema_version\":1,\"sequence_index\":0,"
            + "\"timestamp_boottime_ns\":\"100\","
            + "\"frame_available\":true,"
            + "\"frame_content_type\":\"image/jpeg\","
            + "\"frame_byte_offset\":\"0\",\"frame_byte_length\":6,"
            + "\"model_id\":\"model\\\"alpha\",\"image_rotation_degrees\":0,"
            + "\"inference_duration_ns\":\"7\","
            + "\"person_confidence\":0.75,\"address_confidence\":0.5,"
            + "\"motion_magnitude\":0.0,\"hitting_region_occupied\":true,"
            + "\"controller_state\":\"monitoring\",\"decision_reason\":\"wait\\nnow\"}\n"
            + "{\"schema_version\":1,\"sequence_index\":1,"
            + "\"timestamp_boottime_ns\":\"300\","
            + "\"frame_available\":true,"
            + "\"frame_content_type\":\"image/jpeg\","
            + "\"frame_byte_offset\":\"6\",\"frame_byte_length\":5,"
            + "\"model_id\":\"model-beta\",\"image_rotation_degrees\":0,"
            + "\"inference_duration_ns\":\"11\","
            + "\"person_confidence\":0.9,\"address_confidence\":0.8,"
            + "\"motion_magnitude\":0.125,\"hitting_region_occupied\":true,"
            + "\"controller_state\":\"high_speed_requested\","
            + "\"decision_reason\":\"stable\"}\n";
    byte[] trace =
        Files.readAllBytes(new File(published, PoseDiagnosticFiles.TRACE_FILE_NAME).toPath());
    check(
        new String(trace, StandardCharsets.UTF_8).equals(expectedTrace), "canonical NDJSON trace");
    check(trace.length == metadata.traceBytes(), "trace metadata length");

    String expectedMetadata =
        "{\"schema_version\":1,"
            + "\"frames_path\":\"pose_diagnostics/preview_frames.mjpeg\","
            + "\"frames_content_type\":\"image/jpeg\",\"frames_bytes\":\"11\","
            + "\"trace_path\":\"pose_diagnostics/pose_trace.ndjson\","
            + "\"trace_content_type\":\"application/x-ndjson\","
            + "\"trace_bytes\":"
            + trace.length
            + ",\"first_timestamp_boottime_ns\":\"100\","
            + "\"end_timestamp_boottime_ns_exclusive\":\"301\","
            + "\"observation_count\":2,\"jpeg_frame_count\":2,\"frame_count\":2,"
            + "\"frame_index\":[{\"sequence_index\":0,"
            + "\"timestamp_boottime_ns\":\"100\",\"byte_offset\":\"0\","
            + "\"byte_length\":6,\"content_type\":\"image/jpeg\"},"
            + "{\"sequence_index\":1,\"timestamp_boottime_ns\":\"300\","
            + "\"byte_offset\":\"6\",\"byte_length\":5,"
            + "\"content_type\":\"image/jpeg\"}]}";
    check(metadata.toCanonicalJson().equals(expectedMetadata), "canonical manifest metadata");
  }

  private static void publishesFullRecommendedCountAsTwoFiles() throws Exception {
    File session = temporarySession("full-count");
    PreviewEvidenceRing ring =
        new PreviewEvidenceRing(
            1_000,
            PoseDiagnosticFiles.MAXIMUM_ENTRY_COUNT,
            PoseDiagnosticFiles.MAXIMUM_FRAME_BYTES);
    for (int index = 0; index < PoseDiagnosticFiles.MAXIMUM_ENTRY_COUNT; ++index) {
      ring.append(
          evidence(
              index,
              jpeg(index & 0xff),
              "model",
              1,
              0.5,
              0.5,
              0.0,
              true,
              PreviewEvidence.ControllerState.QUALIFYING,
              "qualifying"));
    }

    PoseDiagnosticFiles.ManifestMetadata metadata =
        new PoseDiagnosticFiles(ignored -> {}).publish(session, ring.snapshot());

    check(metadata.frameCount() == 300, "full recommended frame count");
    File[] files = new File(session, PoseDiagnosticFiles.DIRECTORY_NAME).listFiles();
    check(files != null && files.length == 2, "full snapshot remains two files");
  }

  private static void publishesFiveHzTraceWithLowerCadenceJpegs() throws Exception {
    File session = temporarySession("mixed-cadence");
    PreviewEvidenceRing ring = new PreviewEvidenceRing(10_000, 10, 1_000);
    for (int index = 0; index < 5; ++index) {
      ring.append(
          evidence(
              index * 200L,
              index == 0 || index == 4 ? jpeg(index) : new byte[0],
              "model",
              1,
              0.5,
              0.5,
              0.0,
              true,
              PreviewEvidence.ControllerState.QUALIFYING,
              "five-hz"));
    }
    PoseDiagnosticFiles.ManifestMetadata metadata =
        new PoseDiagnosticFiles(ignored -> {}).publish(session, ring.snapshot());
    check(metadata.observationCount() == 5, "all five-Hz rows represented in manifest");
    check(metadata.frameCount() == 2, "only lower-cadence JPEGs represented in manifest");
    check(metadata.frames().get(1).sequenceIndex() == 4, "frame index maps to observation row");
    String trace =
        Files.readString(
            new File(
                    new File(session, PoseDiagnosticFiles.DIRECTORY_NAME),
                    PoseDiagnosticFiles.TRACE_FILE_NAME)
                .toPath());
    check(trace.lines().count() == 5, "trace has every observation row");
    check(trace.contains("\"frame_available\":false"), "trace marks rows without JPEGs");
    check(trace.contains("\"frame_content_type\":null"), "trace null frame metadata is truthful");
  }

  private static void rejectsEmptyOversizedAndMalformedSnapshots() throws Exception {
    File emptySession = temporarySession("empty");
    PreviewEvidenceRing empty = new PreviewEvidenceRing(1_000, 1, 100);
    expectThrows(
        IllegalArgumentException.class,
        () -> new PoseDiagnosticFiles(ignored -> {}).publish(emptySession, empty.snapshot()),
        "empty snapshot");
    check(emptySession.list().length == 0, "empty rejection has no artifacts");

    File malformedSession = temporarySession("malformed");
    PreviewEvidenceRing malformed = new PreviewEvidenceRing(1_000, 1, 100);
    malformed.append(
        evidence(
            1,
            new byte[] {0, 1, 2, 3},
            "model",
            1,
            0.5,
            0.5,
            0.0,
            true,
            PreviewEvidence.ControllerState.MONITORING,
            "bad jpeg"));
    expectThrows(
        IllegalArgumentException.class,
        () ->
            new PoseDiagnosticFiles(ignored -> {})
                .publish(malformedSession, malformed.snapshot()),
        "malformed JPEG");
    check(malformedSession.list().length == 0, "JPEG rejection has no artifacts");

    File countSession = temporarySession("count");
    PreviewEvidenceRing tooMany =
        new PreviewEvidenceRing(1_000, PoseDiagnosticFiles.MAXIMUM_ENTRY_COUNT + 1, 10_000);
    for (int index = 0; index <= PoseDiagnosticFiles.MAXIMUM_ENTRY_COUNT; ++index) {
      tooMany.append(
          evidence(
              index,
              jpeg(),
              "model",
              1,
              0.5,
              0.5,
              0.0,
              true,
              PreviewEvidence.ControllerState.MONITORING,
              "count"));
    }
    expectThrows(
        IllegalArgumentException.class,
        () -> new PoseDiagnosticFiles(ignored -> {}).publish(countSession, tooMany.snapshot()),
        "entry count bound");
    check(countSession.list().length == 0, "count rejection has no artifacts");
  }

  private static void rejectsUnsafeOrConflictingPaths() throws Exception {
    File root = new File(System.getenv("TEST_TMPDIR"));
    File publishedName = new File(root, "not-temporary");
    check(publishedName.mkdir(), "create non-temporary directory");
    PreviewEvidenceRing ring = singleEntryRing();
    expectThrows(
        IOException.class,
        () ->
            new PoseDiagnosticFiles(ignored -> {}).publish(publishedName, ring.snapshot()),
        "published session path");

    File conflict = temporarySession("conflict");
    File existing = new File(conflict, PoseDiagnosticFiles.DIRECTORY_NAME);
    check(existing.mkdir(), "create conflicting path");
    expectThrows(
        IOException.class,
        () -> new PoseDiagnosticFiles(ignored -> {}).publish(conflict, ring.snapshot()),
        "conflicting evidence path");
    check(existing.isDirectory(), "conflicting path preserved");
  }

  private static void rejectsInconsistentManifestMetadata() {
    PoseDiagnosticFiles.FrameMetadata valid =
        new PoseDiagnosticFiles.FrameMetadata(0, 10, 0, 4, PoseDiagnosticFiles.FRAME_CONTENT_TYPE);
    expectThrows(
        IllegalArgumentException.class,
        () ->
            new PoseDiagnosticFiles.ManifestMetadata(
                1,
                "../preview_frames.mjpeg",
                PoseDiagnosticFiles.FRAME_CONTENT_TYPE,
                4,
                PoseDiagnosticFiles.TRACE_RELATIVE_PATH,
                PoseDiagnosticFiles.TRACE_CONTENT_TYPE,
                1,
                10,
                11,
                1,
                List.of(valid)),
        "unsafe metadata path");
    PoseDiagnosticFiles.FrameMetadata wrongOffset =
        new PoseDiagnosticFiles.FrameMetadata(0, 10, 1, 4, PoseDiagnosticFiles.FRAME_CONTENT_TYPE);
    expectThrows(
        IllegalArgumentException.class,
        () ->
            new PoseDiagnosticFiles.ManifestMetadata(
                1,
                PoseDiagnosticFiles.FRAMES_RELATIVE_PATH,
                PoseDiagnosticFiles.FRAME_CONTENT_TYPE,
                4,
                PoseDiagnosticFiles.TRACE_RELATIVE_PATH,
                PoseDiagnosticFiles.TRACE_CONTENT_TYPE,
                1,
                10,
                11,
                1,
                List.of(wrongOffset)),
        "inconsistent metadata offset");
    PoseDiagnosticFiles.FrameMetadata overBound =
        new PoseDiagnosticFiles.FrameMetadata(
            0,
            10,
            0,
            Math.toIntExact(PoseDiagnosticFiles.MAXIMUM_FRAME_BYTES + 1),
            PoseDiagnosticFiles.FRAME_CONTENT_TYPE);
    expectThrows(
        IllegalArgumentException.class,
        () ->
            new PoseDiagnosticFiles.ManifestMetadata(
                1,
                PoseDiagnosticFiles.FRAMES_RELATIVE_PATH,
                PoseDiagnosticFiles.FRAME_CONTENT_TYPE,
                PoseDiagnosticFiles.MAXIMUM_FRAME_BYTES + 1,
                PoseDiagnosticFiles.TRACE_RELATIVE_PATH,
                PoseDiagnosticFiles.TRACE_CONTENT_TYPE,
                1,
                10,
                11,
                1,
                List.of(overBound)),
        "metadata byte bound");
  }

  private static void failedDirectorySyncCleansStagingUnit() throws Exception {
    File session = temporarySession("sync-failure");
    PoseDiagnosticFiles publisher =
        new PoseDiagnosticFiles(
            ignored -> {
              throw new IOException("synthetic sync failure");
            });

    expectThrows(
        IOException.class,
        () -> publisher.publish(session, singleEntryRing().snapshot()),
        "directory sync failure");
    check(!new File(session, PoseDiagnosticFiles.DIRECTORY_NAME).exists(), "no published unit");
    check(
        !new File(session, PoseDiagnosticFiles.DIRECTORY_NAME + ".tmp").exists(),
        "failed staging unit cleaned");
  }

  private static PreviewEvidenceRing singleEntryRing() {
    PreviewEvidenceRing ring = new PreviewEvidenceRing(100, 1, 100);
    ring.append(
        evidence(
            10,
            jpeg(1),
            "model",
            1,
            0.5,
            0.5,
            0.0,
            true,
            PreviewEvidence.ControllerState.MONITORING,
            "waiting"));
    return ring;
  }

  private static PreviewEvidence evidence(
      long timestamp,
      byte[] frame,
      String model,
      long inferenceDuration,
      double person,
      double address,
      double motion,
      boolean occupied,
      PreviewEvidence.ControllerState state,
      String reason) {
    return new PreviewEvidence(
        timestamp,
        frame,
        model,
        inferenceDuration,
        person,
        address,
        motion,
        occupied,
        state,
        reason);
  }

  private static byte[] jpeg(int... body) {
    byte[] result = new byte[body.length + 4];
    result[0] = (byte) 0xff;
    result[1] = (byte) 0xd8;
    for (int index = 0; index < body.length; ++index) {
      result[index + 2] = (byte) body[index];
    }
    result[result.length - 2] = (byte) 0xff;
    result[result.length - 1] = (byte) 0xd9;
    return result;
  }

  private static File temporarySession(String name) throws IOException {
    File result = new File(System.getenv("TEST_TMPDIR"), name + ".tmp");
    Files.createDirectory(result.toPath());
    return result;
  }

  @FunctionalInterface
  private interface ThrowingRunnable {
    void run() throws Exception;
  }

  private static <T extends Throwable> void expectThrows(
      Class<T> expected, ThrowingRunnable action, String message) {
    try {
      action.run();
    } catch (Throwable thrown) {
      if (expected.isInstance(thrown)) {
        return;
      }
      throw new AssertionError(message + " threw " + thrown, thrown);
    }
    throw new AssertionError(message + " did not throw " + expected.getSimpleName());
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
