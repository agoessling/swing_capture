package com.agoessling.swingcapture;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Comparator;
import java.util.List;

/** Deterministic publication and fault-injection tests for field-recording bundles. */
public final class FieldRecordingPublisherTest {
  private static final String RECORDING_ID = "field-publish-1";

  private FieldRecordingPublisherTest() {}

  public static void main(String[] args) throws Exception {
    publishesCompleteBundleAtomically();
    rejectsMismatchedMediaWithoutMutation();
    cleansEveryPrecommitFailureFromOwnedStaging();
    retainsCompleteBundleWhenFinalDirectorySyncFailsAfterCommit();
  }

  private static void publishesCompleteBundleAtomically() throws Exception {
    try (Fixture fixture = Fixture.create()) {
      FieldRecordingPublisher.PublishedFiles result =
          FieldRecordingPublisher.publish(
              fixture.root,
              fixture.staging,
              fixture.published,
              fixture.videoTemporary,
              fixture.audioTemporary,
              fixture.metadata(),
              fixture.operations());

      check(!fixture.staging.exists(), "staging directory must disappear at commit");
      check(result.directory().equals(fixture.published), "published directory mismatch");
      check(result.video().length() == fixture.videoBytes, "published video length mismatch");
      check(result.audio().length() == fixture.audioBytes, "published audio length mismatch");
      check(
          Files.readString(result.manifest().toPath(), StandardCharsets.UTF_8)
              .equals(FieldRecordingManifest.toCanonicalJson(fixture.metadata())),
          "published manifest mismatch");
    }
  }

  private static void rejectsMismatchedMediaWithoutMutation() throws Exception {
    try (Fixture fixture = Fixture.create()) {
      Files.write(fixture.videoTemporary.toPath(), new byte[] {1, 2});
      expectIOException(
          () ->
              FieldRecordingPublisher.publish(
                  fixture.root,
                  fixture.staging,
                  fixture.published,
                  fixture.videoTemporary,
                  fixture.audioTemporary,
                  fixture.metadata(),
                  fixture.operations()),
          "mismatched video length");
      check(fixture.staging.isDirectory(), "validation failure must preserve staging");
      check(!fixture.published.exists(), "validation failure must not publish a directory");
      check(fixture.videoTemporary.isFile(), "validation failure must not rename video");
      check(fixture.audioTemporary.isFile(), "validation failure must not rename audio");
    }
  }

  private static void cleansEveryPrecommitFailureFromOwnedStaging() throws Exception {
    // Operations 1 through 8 finish with the atomic staging-directory rename. Inject before each.
    for (int failureOperation = 1; failureOperation <= 8; ++failureOperation) {
      try (Fixture fixture = Fixture.create()) {
        FieldRecordingPublisher.Operations operations =
            new FailingOperations(fixture.operations(), failureOperation);
        expectIOException(
            () ->
                FieldRecordingPublisher.publish(
                    fixture.root,
                    fixture.staging,
                    fixture.published,
                    fixture.videoTemporary,
                    fixture.audioTemporary,
                    fixture.metadata(),
                    operations),
            "precommit operation " + failureOperation);
        check(!fixture.published.exists(), "precommit failure exposed a published directory");
        check(
            SessionStagingCleanup.cleanupFailedPublication(fixture.root, fixture.staging),
            "owned staging cleanup failed after operation " + failureOperation);
        check(!fixture.staging.exists(), "cleanup left staging after operation " + failureOperation);
      }
    }
  }

  private static void retainsCompleteBundleWhenFinalDirectorySyncFailsAfterCommit()
      throws Exception {
    try (Fixture fixture = Fixture.create()) {
      FieldRecordingPublisher.Operations operations =
          new FailingOperations(fixture.operations(), 9);
      expectIOException(
          () ->
              FieldRecordingPublisher.publish(
                  fixture.root,
                  fixture.staging,
                  fixture.published,
                  fixture.videoTemporary,
                  fixture.audioTemporary,
                  fixture.metadata(),
                  operations),
          "postcommit root synchronization");
      check(!fixture.staging.exists(), "committed staging path must be absent");
      check(fixture.published.isDirectory(), "postcommit failure lost complete directory");
      check(
          new File(fixture.published, FieldRecordingPublisher.VIDEO_FILE_NAME).isFile(),
          "postcommit video missing");
      check(
          new File(fixture.published, FieldRecordingPublisher.AUDIO_FILE_NAME).isFile(),
          "postcommit audio missing");
      check(
          new File(fixture.published, FieldRecordingPublisher.MANIFEST_FILE_NAME).isFile(),
          "postcommit manifest missing");
    }
  }

  private static FieldRecordingManifest.Data metadata(long videoBytes, long audioFrames) {
    long audioBytes = StreamingPcm16WavFile.expectedFileBytes(audioFrames);
    return new FieldRecordingManifest.Data(
        RECORDING_ID,
        "shared-1",
        "pixel-1",
        "face_on",
        "2026-08-23T00:00:00Z",
        1_000_000_000L,
        2_000_000_000L,
        90,
        "explicit",
        "c2.android.avc.encoder",
        videoBytes,
        10,
        10,
        6,
        audioFrames,
        audioBytes,
        1,
        new FieldRecordingManifest.ClockAnchor(100, 1_100, 102),
        List.of(new FieldRecordingManifest.CameraFrame(1, 1_000)),
        List.of(new FieldRecordingManifest.EncodedSample(0, 10, 0, 1, (int) videoBytes)),
        List.of(new FieldRecordingManifest.AudioTimestampObservation(audioFrames, 0, 1_000, 1)));
  }

  private static void expectIOException(ThrowingRunnable action, String label) throws Exception {
    try {
      action.run();
      throw new AssertionError("Expected IOException for " + label);
    } catch (IOException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }

  private interface ThrowingRunnable {
    void run() throws Exception;
  }

  private static final class FailingOperations implements FieldRecordingPublisher.Operations {
    private final FieldRecordingPublisher.Operations delegate;
    private final int failureOperation;
    private int operation;

    FailingOperations(FieldRecordingPublisher.Operations delegate, int failureOperation) {
      this.delegate = delegate;
      this.failureOperation = failureOperation;
    }

    @Override
    public void synchronizeFile(File file) throws IOException {
      beforeOperation();
      delegate.synchronizeFile(file);
    }

    @Override
    public void writeAndSynchronize(File file, byte[] contents) throws IOException {
      beforeOperation();
      delegate.writeAndSynchronize(file, contents);
    }

    @Override
    public void move(File source, File destination) throws IOException {
      beforeOperation();
      delegate.move(source, destination);
    }

    @Override
    public void synchronizeDirectory(File directory) throws IOException {
      beforeOperation();
      delegate.synchronizeDirectory(directory);
    }

    private void beforeOperation() throws IOException {
      ++operation;
      if (operation == failureOperation) {
        throw new IOException("injected publication failure at operation " + operation);
      }
    }
  }

  private static final class Fixture implements AutoCloseable {
    final Path temporary;
    final File root;
    final File staging;
    final File published;
    final File videoTemporary;
    final File audioTemporary;
    final long videoBytes = 1;
    final long audioFrames = 1;
    final long audioBytes = StreamingPcm16WavFile.expectedFileBytes(audioFrames);

    private Fixture(Path temporary) throws Exception {
      this.temporary = temporary;
      root = Files.createDirectory(temporary.resolve("field_recordings")).toFile();
      staging = Files.createDirectory(root.toPath().resolve(RECORDING_ID + ".tmp")).toFile();
      published = new File(root, RECORDING_ID);
      SessionStagingCleanup.markOwned(root, staging);
      videoTemporary = new File(staging, FieldRecordingPublisher.VIDEO_FILE_NAME + ".tmp");
      audioTemporary = new File(staging, FieldRecordingPublisher.AUDIO_FILE_NAME + ".tmp");
      Files.write(videoTemporary.toPath(), new byte[] {7});
      Files.write(audioTemporary.toPath(), new byte[(int) audioBytes]);
    }

    static Fixture create() throws Exception {
      return new Fixture(Files.createTempDirectory("field-recording-publisher-test"));
    }

    FieldRecordingManifest.Data metadata() {
      return FieldRecordingPublisherTest.metadata(videoBytes, audioFrames);
    }

    FieldRecordingPublisher.Operations operations() {
      return FieldRecordingPublisher.fileSystemOperations(directory -> {});
    }

    @Override
    public void close() throws Exception {
      if (!Files.exists(temporary)) {
        return;
      }
      try (var paths = Files.walk(temporary)) {
        for (Path path : paths.sorted(Comparator.reverseOrder()).toList()) {
          Files.deleteIfExists(path);
        }
      }
    }
  }
}
