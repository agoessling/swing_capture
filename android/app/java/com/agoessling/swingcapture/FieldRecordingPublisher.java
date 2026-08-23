package com.agoessling.swingcapture;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Objects;

/** Atomically publishes a complete field-recording bundle from its owned staging directory. */
final class FieldRecordingPublisher {
  static final String VIDEO_FILE_NAME = "video.mp4";
  static final String AUDIO_FILE_NAME = "audio.wav";
  static final String MANIFEST_FILE_NAME = "manifest.json";

  interface Operations {
    void synchronizeFile(File file) throws IOException;

    void writeAndSynchronize(File file, byte[] contents) throws IOException;

    void move(File source, File destination) throws IOException;

    void synchronizeDirectory(File directory) throws IOException;
  }

  @FunctionalInterface
  interface DirectorySynchronizer {
    void synchronize(File directory) throws IOException;
  }

  record PublishedFiles(File directory, File video, File audio, File manifest) {
    PublishedFiles {
      Objects.requireNonNull(directory, "directory");
      Objects.requireNonNull(video, "video");
      Objects.requireNonNull(audio, "audio");
      Objects.requireNonNull(manifest, "manifest");
    }
  }

  private FieldRecordingPublisher() {}

  static Operations fileSystemOperations(DirectorySynchronizer directorySynchronizer) {
    Objects.requireNonNull(directorySynchronizer, "directorySynchronizer");
    return new Operations() {
      @Override
      public void synchronizeFile(File file) throws IOException {
        try (FileOutputStream output = new FileOutputStream(file, true)) {
          output.getFD().sync();
        }
      }

      @Override
      public void writeAndSynchronize(File file, byte[] contents) throws IOException {
        try (FileOutputStream output = new FileOutputStream(file)) {
          output.write(contents);
          output.getFD().sync();
        }
      }

      @Override
      public void move(File source, File destination) throws IOException {
        if (!source.renameTo(destination)) {
          throw new IOException(
              "Unable to publish " + source.getName() + " as " + destination.getName());
        }
      }

      @Override
      public void synchronizeDirectory(File directory) throws IOException {
        directorySynchronizer.synchronize(directory);
      }
    };
  }

  static PublishedFiles publish(
      File storageRoot,
      File stagingDirectory,
      File publishedDirectory,
      File videoTemporary,
      File audioTemporary,
      FieldRecordingManifest.Data metadata,
      Operations operations)
      throws IOException {
    Objects.requireNonNull(metadata, "metadata");
    Objects.requireNonNull(operations, "operations");
    validatePaths(
        storageRoot, stagingDirectory, publishedDirectory, videoTemporary, audioTemporary, metadata);

    File video = new File(stagingDirectory, VIDEO_FILE_NAME);
    File audio = new File(stagingDirectory, AUDIO_FILE_NAME);
    File temporaryManifest = new File(stagingDirectory, MANIFEST_FILE_NAME + ".tmp");
    File manifest = new File(stagingDirectory, MANIFEST_FILE_NAME);

    operations.synchronizeFile(videoTemporary);
    operations.synchronizeFile(audioTemporary);
    operations.move(videoTemporary, video);
    operations.move(audioTemporary, audio);
    operations.writeAndSynchronize(
        temporaryManifest,
        FieldRecordingManifest.toCanonicalJson(metadata).getBytes(StandardCharsets.UTF_8));
    operations.move(temporaryManifest, manifest);
    operations.synchronizeDirectory(stagingDirectory);
    operations.move(stagingDirectory, publishedDirectory);
    operations.synchronizeDirectory(storageRoot);

    File publishedVideo = new File(publishedDirectory, VIDEO_FILE_NAME);
    File publishedAudio = new File(publishedDirectory, AUDIO_FILE_NAME);
    File publishedManifest = new File(publishedDirectory, MANIFEST_FILE_NAME);
    if (!publishedVideo.isFile() || !publishedAudio.isFile() || !publishedManifest.isFile()) {
      throw new IOException("Published field recording is incomplete");
    }
    return new PublishedFiles(
        publishedDirectory, publishedVideo, publishedAudio, publishedManifest);
  }

  private static void validatePaths(
      File storageRoot,
      File stagingDirectory,
      File publishedDirectory,
      File videoTemporary,
      File audioTemporary,
      FieldRecordingManifest.Data metadata)
      throws IOException {
    requireDirectory(storageRoot, "storage root");
    requireDirectory(stagingDirectory, "staging directory");
    File canonicalRoot = storageRoot.getCanonicalFile();
    File canonicalStaging = stagingDirectory.getCanonicalFile();
    if (!canonicalStaging.getParentFile().equals(canonicalRoot)
        || !stagingDirectory.getName().equals(metadata.recordingId() + ".tmp")) {
      throw new IOException("Field recording staging directory is not a direct owned child");
    }
    if (publishedDirectory.exists()
        || Files.isSymbolicLink(publishedDirectory.toPath())
        || !publishedDirectory
            .getCanonicalFile()
            .getParentFile()
            .equals(canonicalRoot)
        || !publishedDirectory.getName().equals(metadata.recordingId())) {
      throw new IOException("Field recording publication destination is unsafe or occupied");
    }
    requireTemporaryMedia(
        videoTemporary,
        canonicalStaging,
        VIDEO_FILE_NAME + ".tmp",
        metadata.videoBytes(),
        "video");
    requireTemporaryMedia(
        audioTemporary,
        canonicalStaging,
        AUDIO_FILE_NAME + ".tmp",
        metadata.audioBytes(),
        "audio");
    requireAbsent(stagingDirectory, VIDEO_FILE_NAME);
    requireAbsent(stagingDirectory, AUDIO_FILE_NAME);
    requireAbsent(stagingDirectory, MANIFEST_FILE_NAME + ".tmp");
    requireAbsent(stagingDirectory, MANIFEST_FILE_NAME);
  }

  private static void requireDirectory(File directory, String label) throws IOException {
    Objects.requireNonNull(directory, label);
    if (!directory.isDirectory() || Files.isSymbolicLink(directory.toPath())) {
      throw new IOException("Field recording " + label + " is not a regular directory");
    }
  }

  private static void requireTemporaryMedia(
      File file, File canonicalStaging, String expectedName, long expectedBytes, String label)
      throws IOException {
    Objects.requireNonNull(file, label);
    if (!file.isFile()
        || Files.isSymbolicLink(file.toPath())
        || !file.getName().equals(expectedName)
        || !file.getCanonicalFile().getParentFile().equals(canonicalStaging)
        || file.length() != expectedBytes) {
      throw new IOException("Field recording " + label + " staging file is invalid");
    }
  }

  private static void requireAbsent(File parent, String name) throws IOException {
    File target = new File(parent, name);
    if (target.exists() || Files.isSymbolicLink(target.toPath())) {
      throw new IOException("Field recording target already exists: " + name);
    }
  }
}
