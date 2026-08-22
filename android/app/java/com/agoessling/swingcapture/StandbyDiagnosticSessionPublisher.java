package com.agoessling.swingcapture;

import com.agoessling.swingcapture.diagnostics.DiagnosticIncident;
import com.agoessling.swingcapture.standby.StandbyDiagnosticCoordinator;
import com.agoessling.swingcapture.standby.StandbyDiagnosticManifest;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Objects;
import java.util.Optional;

/** Atomic best-effort-preview publisher for one diagnostic-only standby event session. */
public final class StandbyDiagnosticSessionPublisher {
  public static final String MANIFEST_FILE_NAME = "manifest.json";
  private static final int MAXIMUM_FAILURE_TYPE_CHARACTERS = 160;

  @FunctionalInterface
  public interface DirectorySync {
    void synchronize(File directory) throws IOException;
  }

  public record Result(
      File sessionDirectory,
      StandbyDiagnosticManifest manifest,
      DiagnosticIncident incident,
      Optional<String> previewFailureType) {
    public Result {
      Objects.requireNonNull(sessionDirectory, "sessionDirectory");
      Objects.requireNonNull(manifest, "manifest");
      Objects.requireNonNull(incident, "incident");
      Objects.requireNonNull(previewFailureType, "previewFailureType");
      if (previewFailureType.isPresent()
          != (manifest.previewStatus()
              == StandbyDiagnosticManifest.PublicationStatus.PUBLICATION_FAILED)) {
        throw new IllegalArgumentException("preview failure disagrees with manifest status");
      }
    }
  }

  private final DirectorySync directorySync;

  public StandbyDiagnosticSessionPublisher(DirectorySync directorySync) {
    this.directorySync = Objects.requireNonNull(directorySync, "directorySync");
  }

  /**
   * Publishes a new session directory. Preview failures are recorded but never fail valid audio,
   * incident, or manifest publication.
   */
  public Result publish(
      File sessionsDirectory,
      String sessionId,
      String sourceNodeId,
      long createdAtEpochMillis,
      StandbyDiagnosticCoordinator.FrozenEvidence evidence)
      throws IOException {
    Objects.requireNonNull(evidence, "evidence");
    requireIdentifier(sessionId, "sessionId");
    requireIdentifier(sourceNodeId, "sourceNodeId");
    if (createdAtEpochMillis <= 0) {
      throw new IllegalArgumentException("createdAtEpochMillis must be positive");
    }
    File sessions = validateSessionsDirectory(sessionsDirectory);
    File temporary = checkedChild(sessions, sessionId + ".tmp");
    File published = checkedChild(sessions, sessionId);
    if (temporary.exists() || published.exists()) {
      throw new IOException("Standby diagnostic session path already exists");
    }
    if (!temporary.mkdir()) {
      throw new IOException("Unable to create standby diagnostic session staging directory");
    }

    boolean sessionRenamed = false;
    try {
      SessionStagingCleanup.markOwned(sessions, temporary);
      directorySync.synchronize(temporary);
      DiagnosticPcm16WavFile.EvidenceMetadata wavMetadata =
          DiagnosticPcm16WavFile.metadata(
              evidence.audioSnapshot(), evidence.event().markerFramePosition());
      File wav = checkedChild(temporary, StandbyDiagnosticManifest.AUDIO_FILE_NAME);
      writeWavAtomic(temporary, wav, evidence.audioSnapshot(), wavMetadata.bytes());

      StandbyDiagnosticManifest.PublicationStatus previewStatus =
          StandbyDiagnosticManifest.PublicationStatus.NOT_AVAILABLE;
      Optional<StandbyDiagnosticManifest.PreviewArtifact> previewArtifact = Optional.empty();
      Optional<String> previewFailureType = Optional.empty();
      if (evidence.event().previewSnapshot().entryCount() > 0) {
        try {
          PoseDiagnosticFiles.ManifestMetadata pose =
              new PoseDiagnosticFiles(directorySync::synchronize)
                  .publish(temporary, evidence.event().previewSnapshot());
          previewArtifact = Optional.of(toPreviewArtifact(pose));
          previewStatus = StandbyDiagnosticManifest.PublicationStatus.AVAILABLE;
        } catch (Throwable previewFailure) {
          cleanupFailedPreviewPublication(temporary, previewFailure);
          previewStatus = StandbyDiagnosticManifest.PublicationStatus.PUBLICATION_FAILED;
          previewFailureType = Optional.of(boundedFailureType(previewFailure));
        }
      }

      DiagnosticIncident incident =
          StandbyDiagnosticManifest.incidentFor(
              evidence.event(), sessionId, sourceNodeId, createdAtEpochMillis);
      byte[] incidentBytes = incident.toCanonicalJson().getBytes(StandardCharsets.UTF_8);
      File incidentFile = checkedChild(temporary, StandbyDiagnosticManifest.INCIDENT_FILE_NAME);
      writeBytesAtomic(temporary, incidentFile, incidentBytes);

      StandbyDiagnosticManifest manifest =
          new StandbyDiagnosticManifest(
              sessionId,
              createdAtEpochMillis,
              sourceNodeId,
              StandbyDiagnosticManifest.EventMetadata.from(evidence.event()),
              toAudioArtifact(wavMetadata),
              previewStatus,
              previewArtifact,
              StandbyDiagnosticManifest.INCIDENT_FILE_NAME,
              incidentBytes.length);
      byte[] manifestBytes =
          (manifest.toCanonicalJson() + "\n").getBytes(StandardCharsets.UTF_8);
      File manifestFile = checkedChild(temporary, MANIFEST_FILE_NAME);
      writeBytesAtomic(temporary, manifestFile, manifestBytes);

      directorySync.synchronize(temporary);
      atomicMove(temporary, published);
      sessionRenamed = true;
      directorySync.synchronize(sessions);
      return new Result(published, manifest, incident, previewFailureType);
    } catch (IOException | RuntimeException | Error failure) {
      if (!sessionRenamed && temporary.exists()) {
        if (!SessionStagingCleanup.cleanupFailedPublication(sessions, temporary)) {
          if (!SessionStagingCleanup.cleanupAfterMarkFailure(sessions, temporary)) {
            try {
              deleteOwnedTemporarySession(temporary);
            } catch (IOException cleanupFailure) {
              failure.addSuppressed(cleanupFailure);
            }
          }
        }
      }
      throw failure;
    }
  }

  private void writeWavAtomic(
      File parent,
      File target,
      com.agoessling.swingcapture.diagnostics.DiagnosticAudioRing.Snapshot snapshot,
      long expectedBytes)
      throws IOException {
    File staging = checkedChild(parent, target.getName() + ".tmp");
    requireAbsent(staging, target);
    try {
      try (FileOutputStream fileOutput = new FileOutputStream(staging);
          BufferedOutputStream output = new BufferedOutputStream(fileOutput)) {
        DiagnosticPcm16WavFile.write(snapshot, output);
        output.flush();
        fileOutput.getFD().sync();
      }
      if (staging.length() != expectedBytes) {
        throw new IOException("Standby WAV bytes disagree with metadata");
      }
      atomicMove(staging, target);
    } finally {
      Files.deleteIfExists(staging.toPath());
    }
  }

  private void writeBytesAtomic(File parent, File target, byte[] contents) throws IOException {
    File staging = checkedChild(parent, target.getName() + ".tmp");
    requireAbsent(staging, target);
    try {
      try (FileOutputStream output = new FileOutputStream(staging)) {
        output.write(contents);
        output.getFD().sync();
      }
      if (staging.length() != contents.length) {
        throw new IOException("Standby diagnostic file bytes changed during publication");
      }
      atomicMove(staging, target);
    } finally {
      Files.deleteIfExists(staging.toPath());
    }
  }

  private static StandbyDiagnosticManifest.AudioArtifact toAudioArtifact(
      DiagnosticPcm16WavFile.EvidenceMetadata metadata) {
    return new StandbyDiagnosticManifest.AudioArtifact(
        metadata.relativePath(),
        StandbyDiagnosticManifest.AUDIO_CONTENT_TYPE,
        metadata.bytes(),
        metadata.sampleRateHz(),
        metadata.firstFramePosition(),
        metadata.endFramePosition(),
        metadata.markerFramePosition(),
        metadata.sampleCount(),
        metadata.markerSampleIndex());
  }

  private static StandbyDiagnosticManifest.PreviewArtifact toPreviewArtifact(
      PoseDiagnosticFiles.ManifestMetadata metadata) {
    return new StandbyDiagnosticManifest.PreviewArtifact(
        metadata.framesRelativePath(),
        metadata.framesContentType(),
        metadata.framesBytes(),
        metadata.traceRelativePath(),
        metadata.traceContentType(),
        metadata.traceBytes(),
        metadata.firstTimestampInclusive(),
        metadata.endTimestampExclusive(),
        metadata.observationCount(),
        metadata.frameCount());
  }

  private static File validateSessionsDirectory(File directory) throws IOException {
    Objects.requireNonNull(directory, "sessionsDirectory");
    if ((!directory.isDirectory() && !directory.mkdirs())
        || Files.isSymbolicLink(directory.toPath())) {
      throw new IOException("Standby diagnostics require a safe sessions directory");
    }
    return directory.getCanonicalFile();
  }

  private static File checkedChild(File parent, String name) throws IOException {
    if (!name.matches("[A-Za-z0-9._-]+")) {
      throw new IllegalArgumentException("Standby diagnostic filename is unsafe");
    }
    File child = new File(parent, name);
    if (!child.getCanonicalFile().getParentFile().equals(parent.getCanonicalFile())
        || Files.isSymbolicLink(child.toPath())) {
      throw new IOException("Standby diagnostic path escapes its parent");
    }
    return child;
  }

  private static void requireAbsent(File temporary, File target) throws IOException {
    if (temporary.exists() || target.exists()) {
      throw new IOException("Standby diagnostic file path already exists");
    }
  }

  private static void atomicMove(File temporary, File target) throws IOException {
    try {
      Files.move(temporary.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE);
    } catch (AtomicMoveNotSupportedException unsupported) {
      Files.move(temporary.toPath(), target.toPath());
    }
  }

  private static void deleteOwnedTemporarySession(File temporary) throws IOException {
    if (Files.isSymbolicLink(temporary.toPath()) || !temporary.isDirectory()) {
      throw new IOException("Refusing to clean changed standby diagnostic staging path");
    }
    File[] children = temporary.listFiles();
    if (children == null) {
      throw new IOException("Unable to inspect standby diagnostic staging directory");
    }
    for (File child : children) {
      if (!child.getCanonicalPath().startsWith(temporary.getCanonicalPath() + File.separator)
          || Files.isSymbolicLink(child.toPath())) {
        throw new IOException("Refusing to clean unsafe standby diagnostic staging entry");
      }
      if (child.isDirectory()) {
        deleteOwnedPoseDirectory(child, child.getName());
      } else {
        Files.delete(child.toPath());
      }
    }
    Files.delete(temporary.toPath());
  }

  private static void cleanupFailedPreviewPublication(File temporary, Throwable previewFailure) {
    for (String name :
        new String[] {
          PoseDiagnosticFiles.DIRECTORY_NAME, PoseDiagnosticFiles.DIRECTORY_NAME + ".tmp"
        }) {
      File directory = new File(temporary, name);
      if (!directory.exists()) {
        continue;
      }
      try {
        deleteOwnedPoseDirectory(directory, name);
      } catch (IOException cleanupFailure) {
        previewFailure.addSuppressed(cleanupFailure);
      }
    }
  }

  private static void deleteOwnedPoseDirectory(File directory, String expectedName)
      throws IOException {
    if (!directory.getName().equals(expectedName)
        || (!expectedName.equals(PoseDiagnosticFiles.DIRECTORY_NAME)
            && !expectedName.equals(PoseDiagnosticFiles.DIRECTORY_NAME + ".tmp"))
        || Files.isSymbolicLink(directory.toPath())
        || !directory.isDirectory()) {
      throw new IOException("Unexpected standby diagnostic staging directory");
    }
    File[] children = directory.listFiles();
    if (children == null) {
      throw new IOException("Unable to inspect pose diagnostic staging directory");
    }
    for (File child : children) {
      if ((!child.getName().equals(PoseDiagnosticFiles.FRAMES_FILE_NAME)
              && !child.getName().equals(PoseDiagnosticFiles.TRACE_FILE_NAME))
          || !child.isFile()
          || Files.isSymbolicLink(child.toPath())) {
        throw new IOException("Unexpected pose diagnostic staging entry");
      }
      Files.delete(child.toPath());
    }
    Files.delete(directory.toPath());
  }

  private static String boundedFailureType(Throwable failure) {
    String type = failure.getClass().getName();
    return type.length() <= MAXIMUM_FAILURE_TYPE_CHARACTERS
        ? type
        : type.substring(0, MAXIMUM_FAILURE_TYPE_CHARACTERS);
  }

  private static void requireIdentifier(String value, String name) {
    Objects.requireNonNull(value, name);
    if (!value.matches("[A-Za-z0-9._-]+")
        || value.getBytes(StandardCharsets.UTF_8).length
            > DiagnosticIncident.MAXIMUM_IDENTIFIER_BYTES) {
      throw new IllegalArgumentException(name + " is invalid");
    }
  }
}
