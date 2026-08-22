package com.agoessling.swingcapture;

import java.io.File;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.time.Instant;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** Host coverage for complete, bounded, and path-safe diagnostic ZIP publication. */
public final class SessionDiagnosticArchiveTest {
  private SessionDiagnosticArchiveTest() {}

  public static void main(String[] arguments) throws Exception {
    archiveContainsChecksummedSessionEvidenceAndExcludesTemporaryFiles();
    traceOnlyPoseEvidencePreservesZeroBytePreview();
    ownershipMarkerIsAbsentFromCanonicalDiagnosticExport();
    rejectsNestedOwnershipMarker();
    exactDestinationReplacementIsDeterministic();
    rejectsUnsafeSourcesAndDestinations();
    rejectsDuplicateGeneratedPathsAndOversizedSources();
  }

  private static void traceOnlyPoseEvidencePreservesZeroBytePreview() throws Exception {
    File root = testDirectory("trace-only-pose");
    File session = new File(root, "session-trace-only");
    check(session.mkdir(), "create trace-only session");
    Files.writeString(
        new File(session, "manifest.json").toPath(),
        "{\"pose_preview\":{\"available\":true,\"frames_bytes\":\"0\","
            + "\"observation_count\":3,\"jpeg_frame_count\":0}}\n",
        StandardCharsets.UTF_8);
    File poseDiagnostics = new File(session, "pose_diagnostics");
    check(poseDiagnostics.mkdir(), "create trace-only pose diagnostics");
    File emptyFrames = new File(poseDiagnostics, "preview_frames.mjpeg");
    Files.write(emptyFrames.toPath(), new byte[0]);
    File trace = new File(poseDiagnostics, "pose_trace.ndjson");
    Files.writeString(
        trace.toPath(),
        "{\"sequence_index\":0,\"frame_available\":false}\n",
        StandardCharsets.UTF_8);

    File destination = new File(new File(root, "exports"), "trace-only.zip");
    SessionDiagnosticArchive.Result result =
        SessionDiagnosticArchive.create(
            session, destination, Instant.parse("2026-08-21T23:00:00Z"));
    check(result.fileCount() == 3, "trace-only archive retains all declared regular files");
    check(
        result.sourceBytes()
            == new File(session, "manifest.json").length() + trace.length(),
        "zero-byte preview contributes zero to source byte count");

    try (ZipFile zip = new ZipFile(destination)) {
      ZipEntry framesEntry = zip.getEntry("session-trace-only/pose_diagnostics/preview_frames.mjpeg");
      check(framesEntry != null, "trace-only archive retains declared preview path");
      check(framesEntry.getSize() == 0, "trace-only preview ZIP entry remains empty");
      check(zip.getInputStream(framesEntry).readAllBytes().length == 0, "preview payload is empty");
      String export =
          new String(
              zip.getInputStream(zip.getEntry("diagnostic_export.json")).readAllBytes(),
              StandardCharsets.UTF_8);
      check(
          export.contains(
              "\"path\":\"session-trace-only/pose_diagnostics/preview_frames.mjpeg\","
                  + "\"bytes\":0,\"sha256\":"
                  + "\"e3b0c44298fc1c149afbf4c8996fb924"
                  + "27ae41e4649b934ca495991b7852b855\""),
          "export manifest declares SHA-256(empty) for preview");
      check(
          zip.getEntry("session-trace-only/pose_diagnostics/pose_trace.ndjson") != null,
          "trace remains exportable when every JPEG encoding failed");
    }
  }

  private static void ownershipMarkerIsAbsentFromCanonicalDiagnosticExport() throws Exception {
    File root = testDirectory("ownership-marker");
    File staging = new File(root, "session-field.tmp");
    check(staging.mkdir(), "create owned staging session");
    SessionStagingCleanup.markOwned(root, staging);
    Files.writeString(new File(staging, "manifest.json").toPath(), "{}\n");
    Files.write(new File(staging, "diagnostic_audio.wav").toPath(), new byte[] {1});
    Files.writeString(new File(staging, "diagnostic_incident.json").toPath(), "{}\n");
    File poseDiagnostics = new File(staging, "pose_diagnostics");
    check(poseDiagnostics.mkdir(), "create pose diagnostics directory");
    Files.write(new File(poseDiagnostics, "preview_frames.mjpeg").toPath(), new byte[] {2});
    Files.writeString(new File(poseDiagnostics, "pose_trace.ndjson").toPath(), "{}\n");

    File session = new File(root, "session-field");
    Files.move(staging.toPath(), session.toPath());
    check(
        new File(session, SessionStagingCleanup.OWNERSHIP_MARKER).isFile(),
        "published fixture retains the real ownership marker");
    File destination = new File(new File(root, "exports"), "session-field.zip");
    SessionDiagnosticArchive.Result result =
        SessionDiagnosticArchive.create(
            session, destination, Instant.parse("2026-08-21T23:00:00Z"));

    File ownershipMarker = new File(session, SessionStagingCleanup.OWNERSHIP_MARKER);
    File publishedPoseDiagnostics = new File(session, "pose_diagnostics");
    long expectedSourceBytes =
        new File(session, "manifest.json").length()
            + new File(session, "diagnostic_audio.wav").length()
            + new File(session, "diagnostic_incident.json").length()
            + new File(publishedPoseDiagnostics, "preview_frames.mjpeg").length()
            + new File(publishedPoseDiagnostics, "pose_trace.ndjson").length();
    check(result.fileCount() == 5, "only five canonical session files are exported");
    check(ownershipMarker.isFile(), "archive creation preserves the ownership marker");
    check(ownershipMarker.length() == 14, "real ownership marker has expected size");
    check(result.sourceBytes() == expectedSourceBytes, "source bytes exclude ownership marker");
    try (ZipFile zip = new ZipFile(destination)) {
      Set<String> names = new HashSet<>();
      zip.stream().map(ZipEntry::getName).forEach(names::add);
      Set<String> expectedNames =
          Set.of(
              "diagnostic_export.json",
              "session-field/manifest.json",
              "session-field/diagnostic_audio.wav",
              "session-field/diagnostic_incident.json",
              "session-field/pose_diagnostics/preview_frames.mjpeg",
              "session-field/pose_diagnostics/pose_trace.ndjson");
      check(names.equals(expectedNames), "ZIP has exactly six canonical entries");
      check(
          !names.contains("session-field/" + SessionStagingCleanup.OWNERSHIP_MARKER),
          "ownership marker is absent from ZIP entries");
      String export =
          new String(
              zip.getInputStream(zip.getEntry("diagnostic_export.json")).readAllBytes(),
              StandardCharsets.UTF_8);
      check(
          !export.contains(SessionStagingCleanup.OWNERSHIP_MARKER),
          "ownership marker is absent from export manifest");
      check(countOccurrences(export, "\"path\":") == 5, "export manifest lists five files");
    }
  }

  private static void rejectsNestedOwnershipMarker() throws Exception {
    File root = testDirectory("nested-ownership-marker");
    File session = new File(root, "session-nested");
    check(session.mkdir(), "create nested-marker session");
    Files.writeString(new File(session, "manifest.json").toPath(), "{}\n");
    File diagnostics = new File(session, "diagnostics");
    check(diagnostics.mkdir(), "create nested-marker diagnostics");
    File nestedMarker = new File(diagnostics, SessionStagingCleanup.OWNERSHIP_MARKER);
    Files.writeString(nestedMarker.toPath(), "swing-capture\n", StandardCharsets.US_ASCII);
    File destination = new File(root, "nested-marker.zip");

    expectIo(
        () ->
            SessionDiagnosticArchive.create(
                session, destination, Instant.parse("2026-08-21T23:00:00Z")),
        "nested ownership marker");
    check(nestedMarker.isFile(), "rejected nested marker is not mutated");
    check(!destination.exists(), "nested ownership marker leaves no archive");
  }

  private static void archiveContainsChecksummedSessionEvidenceAndExcludesTemporaryFiles()
      throws Exception {
    File root = testDirectory("archive");
    File session = new File(root, "session-1");
    check(session.mkdirs(), "create session");
    Files.writeString(
        new File(session, "manifest.json").toPath(),
        "{\"session_id\":\"session-1\"}\n",
        StandardCharsets.UTF_8);
    Files.write(new File(session, "face_on.mp4").toPath(), new byte[] {0, 1, 2, 3});
    File diagnostics = new File(session, "diagnostics");
    check(diagnostics.mkdir(), "create nested diagnostics");
    Files.writeString(
        new File(diagnostics, "feedback.json").toPath(), "{}\n", StandardCharsets.UTF_8);
    Files.writeString(
        new File(session, "ignored.tmp").toPath(), "partial", StandardCharsets.UTF_8);

    File destination = new File(new File(root, "exports"), "session-1.zip");
    byte[] coordination = "{\"status\":\"paired\"}\n".getBytes(StandardCharsets.UTF_8);
    SessionDiagnosticArchive.Result result =
        SessionDiagnosticArchive.create(
            session,
            destination,
            Instant.parse("2026-08-16T19:00:00Z"),
            Map.of("coordination/paired.json", coordination));
    long expectedSourceBytes =
        new File(session, "manifest.json").length()
            + new File(session, "face_on.mp4").length()
            + new File(diagnostics, "feedback.json").length()
            + coordination.length;
    check(result.fileCount() == 4, "four published and generated evidence files");
    check(result.sourceBytes() == expectedSourceBytes, "source byte count");
    check(result.archive().equals(destination), "destination result");
    check(result.archiveBytes() == destination.length(), "archive byte count");

    try (ZipFile zip = new ZipFile(destination)) {
      Set<String> names = new HashSet<>();
      zip.stream().map(ZipEntry::getName).forEach(names::add);
      check(names.contains("diagnostic_export.json"), "export manifest");
      check(names.contains("session-1/manifest.json"), "session manifest");
      check(names.contains("session-1/face_on.mp4"), "video");
      check(names.contains("session-1/diagnostics/feedback.json"), "nested feedback");
      check(names.contains("coordination/paired.json"), "supplemental coordination evidence");
      check(!names.contains("session-1/ignored.tmp"), "temporary file excluded");
      String export =
          new String(
              zip.getInputStream(zip.getEntry("diagnostic_export.json")).readAllBytes(),
              StandardCharsets.UTF_8);
      check(export.contains("\"schema_version\":1"), "export schema");
      check(export.contains("\"session_id\":\"session-1\""), "export session id");
      check(
          export.contains("\"path\":\"coordination/paired.json\""),
          "manifest uses exact supplemental ZIP path");
      check(
          export.contains("\"path\":\"session-1/manifest.json\""),
          "manifest uses exact session ZIP path");
      check(
          export.contains(
              "054edec1d0211f624fed0cbca9d4f9400b0e491c43742af2c5b0abebf0c990d8"),
          "video SHA-256");
    }
  }

  private static void exactDestinationReplacementIsDeterministic() throws Exception {
    File root = testDirectory("deterministic");
    File session = new File(root, "session-a");
    check(session.mkdir(), "create deterministic session");
    Files.writeString(
        new File(session, "manifest.json").toPath(), "{}\n", StandardCharsets.UTF_8);
    Files.write(new File(session, "clip.mp4").toPath(), new byte[] {4, 3, 2, 1});
    File destination = new File(new File(root, "exports"), "request-17.zip");
    Instant createdAt = Instant.parse("2026-08-16T19:00:00Z");

    SessionDiagnosticArchive.Result first =
        SessionDiagnosticArchive.create(
            session,
            destination,
            createdAt,
            Map.of("coordination/paired.json", "paired\n".getBytes(StandardCharsets.UTF_8)));
    byte[] firstBytes = Files.readAllBytes(first.archive().toPath());
    SessionDiagnosticArchive.Result second =
        SessionDiagnosticArchive.create(
            session,
            destination,
            createdAt,
            Map.of("coordination/paired.json", "paired\n".getBytes(StandardCharsets.UTF_8)));
    byte[] secondBytes = Files.readAllBytes(second.archive().toPath());

    check(first.archive().equals(destination), "first exact destination");
    check(second.archive().equals(destination), "replacement exact destination");
    check(Arrays.equals(firstBytes, secondBytes), "identical input produces identical ZIP bytes");
    check(!new File(destination.getParentFile(), destination.getName() + ".tmp").exists(),
        "temporary archive removed");
    File[] outputs = destination.getParentFile().listFiles();
    check(outputs != null && outputs.length == 1 && outputs[0].equals(destination),
        "archive does not allocate suffix destinations");
  }

  private static void rejectsUnsafeSourcesAndDestinations() throws Exception {
    File root = testDirectory("unsafe");
    File unpublished = new File(root, "unpublished");
    check(unpublished.mkdirs(), "create unpublished directory");
    expectIo(
        () ->
            SessionDiagnosticArchive.create(
                unpublished,
                new File(root, "unpublished.zip"),
                Instant.parse("2026-08-16T19:00:00Z")),
        "missing manifest");

    File session = new File(root, "published");
    check(session.mkdir(), "create published directory");
    Files.writeString(new File(session, "manifest.json").toPath(), "{}\n");
    expectIo(
        () ->
            SessionDiagnosticArchive.create(
                session,
                new File(session, "diagnostics.zip"),
                Instant.parse("2026-08-16T19:00:00Z")),
        "destination inside session");
    expectIo(
        () ->
            SessionDiagnosticArchive.create(
                session,
                new File(root, "bad name.zip"),
                Instant.parse("2026-08-16T19:00:00Z")),
        "unsafe destination filename");

    Files.writeString(new File(session, "bad name.txt").toPath(), "unsafe");
    expectIo(
        () ->
            SessionDiagnosticArchive.create(
                session,
                new File(root, "unsafe-source.zip"),
                Instant.parse("2026-08-16T19:00:00Z")),
        "unsafe source filename");
  }

  private static void rejectsDuplicateGeneratedPathsAndOversizedSources() throws Exception {
    File root = testDirectory("generated-bounds");
    File session = new File(root, "bounded");
    check(session.mkdir(), "create bounded session");
    Files.writeString(new File(session, "manifest.json").toPath(), "{}\n");
    File destination = new File(root, "bounded.zip");
    Instant createdAt = Instant.parse("2026-08-16T19:00:00Z");

    expectIo(
        () ->
            SessionDiagnosticArchive.create(
                session,
                destination,
                createdAt,
                Map.of(
                    "bounded/manifest.json",
                    "duplicate\n".getBytes(StandardCharsets.UTF_8))),
        "generated path collides with session path");
    expectInvalid(
        () ->
            SessionDiagnosticArchive.create(
                session,
                destination,
                createdAt,
                Map.of("../coordination.json", new byte[] {1})),
        "generated path traversal");
    expectInvalid(
        () ->
            SessionDiagnosticArchive.create(
                session,
                destination,
                createdAt,
                Map.of(SessionDiagnosticArchive.EXPORT_MANIFEST_NAME, new byte[] {1})),
        "generated path shadows export manifest");

    try (RandomAccessFile sparse = new RandomAccessFile(new File(session, "oversized.bin"), "rw")) {
      sparse.setLength(SessionDiagnosticArchive.MAXIMUM_TOTAL_BYTES + 1);
    }
    expectIo(
        () -> SessionDiagnosticArchive.create(session, destination, createdAt),
        "128 MiB source bound");
    check(!destination.exists(), "failed bounds leave no archive");
  }

  private static File testDirectory(String name) throws Exception {
    File root = new File(System.getenv("TEST_TMPDIR"), name);
    check(root.mkdirs(), "create test root " + name);
    return root;
  }

  private static int countOccurrences(String text, String needle) {
    int count = 0;
    int offset = 0;
    while ((offset = text.indexOf(needle, offset)) >= 0) {
      ++count;
      offset += needle.length();
    }
    return count;
  }

  private static void expectIo(ThrowingAction action, String label) throws Exception {
    try {
      action.run();
      throw new AssertionError("Expected IOException: " + label);
    } catch (java.io.IOException expected) {
      // Expected.
    }
  }

  private static void expectInvalid(ThrowingAction action, String label) throws Exception {
    try {
      action.run();
      throw new AssertionError("Expected IllegalArgumentException: " + label);
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
  private interface ThrowingAction {
    void run() throws Exception;
  }
}
