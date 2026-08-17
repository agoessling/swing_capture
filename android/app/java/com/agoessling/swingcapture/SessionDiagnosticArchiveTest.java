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
    exactDestinationReplacementIsDeterministic();
    rejectsUnsafeSourcesAndDestinations();
    rejectsDuplicateGeneratedPathsAndOversizedSources();
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
