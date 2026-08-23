package com.agoessling.swingcapture;

import com.agoessling.swingcapture.core.coordination.AutonomousPairLifecycle;
import com.agoessling.swingcapture.core.coordination.PairedCoordinationRecord;
import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Objects;
import java.util.Optional;

/** Atomic lifecycle checkpoint and create-only immutable peer-replication backlog. */
final class AutonomousPairDurableStore {
  @FunctionalInterface
  interface DirectorySynchronizer {
    void synchronize(File directory) throws IOException;
  }

  enum EnqueueStatus {
    STORED,
    ALREADY_PRESENT,
    CONFLICT
  }

  record Recovery(
      AutonomousPairLifecycle.Checkpoint checkpoint,
      Optional<PairedCoordinationRecord> pendingRecord) {
    Recovery {
      Objects.requireNonNull(checkpoint, "checkpoint");
      Objects.requireNonNull(pendingRecord, "pendingRecord");
      if (pendingRecord.isPresent()
          && !checkpoint.activeSessionId().equals(
              pendingRecord.orElseThrow().sharedSessionId())) {
        throw new IllegalArgumentException(
            "pending coordination record belongs to another shared session");
      }
    }
  }

  private static final int CHECKPOINT_MAGIC = 0x53434150; // SCAP
  private static final int CHECKPOINT_SCHEMA = 1;
  private static final int MAXIMUM_BACKLOG_RECORDS = 256;
  private static final int MAXIMUM_STRING_BYTES = 128 * 1024;
  private static final String CHECKPOINT_FILE_NAME = "checkpoint.bin";
  private static final String BACKLOG_DIRECTORY_NAME = "replication_backlog";

  private final File root;
  private final File backlogRoot;
  private final DirectorySynchronizer directorySynchronizer;

  AutonomousPairDurableStore(File root, DirectorySynchronizer directorySynchronizer)
      throws IOException {
    this.root = Objects.requireNonNull(root, "root").getCanonicalFile();
    this.backlogRoot = new File(this.root, BACKLOG_DIRECTORY_NAME).getCanonicalFile();
    this.directorySynchronizer =
        Objects.requireNonNull(directorySynchronizer, "directorySynchronizer");
    if (!this.backlogRoot.getParentFile().equals(this.root)) {
      throw new IOException("autonomous-pair backlog escapes its app-private root");
    }
    requireSafeDirectoryOrMissing(this.root, "autonomous-pair root");
    requireSafeDirectoryOrMissing(this.backlogRoot, "autonomous-pair backlog");
  }

  synchronized Optional<Recovery> loadCheckpoint() throws IOException {
    File checkpoint = checkpointFile();
    if (!checkpoint.exists()) {
      return Optional.empty();
    }
    requireRegularFile(checkpoint, "autonomous-pair checkpoint");
    try (DataInputStream input =
        new DataInputStream(new BufferedInputStream(new FileInputStream(checkpoint)))) {
      if (input.readInt() != CHECKPOINT_MAGIC || input.readInt() != CHECKPOINT_SCHEMA) {
        throw new IOException("unsupported autonomous-pair checkpoint format");
      }
      AutonomousPairLifecycle.Checkpoint value =
          new AutonomousPairLifecycle.Checkpoint(
              input.readInt(),
              readEnum(input, AutonomousPairLifecycle.State.class, "lifecycle state"),
              readString(input, 128),
              readString(input, 128),
              readString(input, 128),
              input.readBoolean(),
              input.readBoolean(),
              input.readBoolean(),
              input.readBoolean(),
              input.readBoolean(),
              input.readBoolean(),
              input.readBoolean(),
              input.readBoolean(),
              input.readLong(),
              readEnum(input, AutonomousPairLifecycle.LastOutcome.class, "last outcome"),
              readString(input, 128),
              readString(input, 1_024));
      Optional<PairedCoordinationRecord> pending = Optional.empty();
      if (input.readBoolean()) {
        pending = Optional.of(PairedCoordinationRecord.fromJson(readString(input, 32 * 1024)));
      }
      if (input.read() != -1) {
        throw new IOException("autonomous-pair checkpoint has trailing data");
      }
      return Optional.of(new Recovery(value, pending));
    } catch (EOFException | IllegalArgumentException malformed) {
      throw new IOException("autonomous-pair checkpoint is malformed", malformed);
    }
  }

  synchronized void saveCheckpoint(
      AutonomousPairLifecycle.Checkpoint checkpoint,
      Optional<PairedCoordinationRecord> pendingRecord)
      throws IOException {
    Recovery recovery = new Recovery(checkpoint, pendingRecord);
    ensureDirectory(root);
    File temporary = File.createTempFile("checkpoint-", ".tmp", root);
    try {
      try (FileOutputStream file = new FileOutputStream(temporary);
          DataOutputStream output = new DataOutputStream(file)) {
        output.writeInt(CHECKPOINT_MAGIC);
        output.writeInt(CHECKPOINT_SCHEMA);
        AutonomousPairLifecycle.Checkpoint value = recovery.checkpoint();
        output.writeInt(value.schemaVersion());
        writeString(output, value.state().name());
        writeString(output, value.activeSessionId());
        writeString(output, value.localSessionId());
        writeString(output, value.peerSessionId());
        output.writeBoolean(value.peerAvailable());
        output.writeBoolean(value.peerArmed());
        output.writeBoolean(value.peerArmUnconfirmed());
        output.writeBoolean(value.localTriggered());
        output.writeBoolean(value.peerTriggered());
        output.writeBoolean(value.localPublished());
        output.writeBoolean(value.peerPublished());
        output.writeBoolean(value.clockFresh());
        output.writeLong(value.evidenceDeadlineNanos());
        writeString(output, value.lastOutcome().name());
        writeString(output, value.lastCompletedSessionId());
        writeString(output, value.diagnostic());
        output.writeBoolean(recovery.pendingRecord().isPresent());
        if (recovery.pendingRecord().isPresent()) {
          writeString(output, recovery.pendingRecord().orElseThrow().toJson());
        }
        output.flush();
        file.getFD().sync();
      }
      replaceAtomically(temporary, checkpointFile());
      directorySynchronizer.synchronize(root);
    } finally {
      Files.deleteIfExists(temporary.toPath());
    }
  }

  synchronized void clearCheckpoint() throws IOException {
    File checkpoint = checkpointFile();
    if (checkpoint.exists()) {
      requireRegularFile(checkpoint, "autonomous-pair checkpoint");
      Files.delete(checkpoint.toPath());
      directorySynchronizer.synchronize(root);
    }
  }

  synchronized EnqueueStatus enqueue(PairedCoordinationRecord record) throws IOException {
    Objects.requireNonNull(record, "record");
    ensureDirectory(root);
    ensureDirectory(backlogRoot);
    File target = backlogFile(record.sharedSessionId());
    if (target.exists()) {
      return readBacklogFile(target).equals(record)
          ? EnqueueStatus.ALREADY_PRESENT
          : EnqueueStatus.CONFLICT;
    }
    if (backlog().size() >= MAXIMUM_BACKLOG_RECORDS) {
      throw new IOException("autonomous-pair replication backlog is full");
    }
    File temporary = File.createTempFile("backlog-", ".tmp", backlogRoot);
    try {
      byte[] encoded = record.toJson().getBytes(StandardCharsets.UTF_8);
      try (FileOutputStream output = new FileOutputStream(temporary)) {
        output.write(encoded);
        output.flush();
        output.getFD().sync();
      }
      try {
        Files.move(temporary.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE);
      } catch (AtomicMoveNotSupportedException unsupported) {
        Files.move(temporary.toPath(), target.toPath());
      } catch (java.nio.file.FileAlreadyExistsException raced) {
        return readBacklogFile(target).equals(record)
            ? EnqueueStatus.ALREADY_PRESENT
            : EnqueueStatus.CONFLICT;
      }
      directorySynchronizer.synchronize(backlogRoot);
      return EnqueueStatus.STORED;
    } finally {
      Files.deleteIfExists(temporary.toPath());
    }
  }

  synchronized List<PairedCoordinationRecord> backlog() throws IOException {
    requireSafeDirectoryOrMissing(backlogRoot, "autonomous-pair backlog");
    if (!backlogRoot.isDirectory()) {
      return List.of();
    }
    File[] files = backlogRoot.listFiles((ignored, name) -> name.endsWith(".json"));
    if (files == null) {
      throw new IOException("unable to list autonomous-pair replication backlog");
    }
    if (files.length > MAXIMUM_BACKLOG_RECORDS) {
      throw new IOException("autonomous-pair replication backlog exceeds its bounded capacity");
    }
    List<File> ordered = new ArrayList<>(List.of(files));
    ordered.sort(Comparator.comparing(File::getName));
    List<PairedCoordinationRecord> records = new ArrayList<>(ordered.size());
    for (File file : ordered) {
      PairedCoordinationRecord record = readBacklogFile(file);
      if (!file.getName().equals(record.sharedSessionId() + ".json")) {
        throw new IOException("replication backlog filename disagrees with its record");
      }
      records.add(record);
    }
    return List.copyOf(records);
  }

  synchronized void markReplicated(PairedCoordinationRecord record) throws IOException {
    Objects.requireNonNull(record, "record");
    File target = backlogFile(record.sharedSessionId());
    if (!target.exists()) {
      return;
    }
    if (!readBacklogFile(target).equals(record)) {
      throw new IOException("refusing to remove conflicting immutable backlog evidence");
    }
    Files.delete(target.toPath());
    directorySynchronizer.synchronize(backlogRoot);
  }

  synchronized int backlogSize() throws IOException {
    return backlog().size();
  }

  private PairedCoordinationRecord readBacklogFile(File file) throws IOException {
    requireRegularFile(file, "autonomous-pair backlog record");
    if (file.length() > PairedCoordinationRecord.MAXIMUM_SERIALIZED_BYTES) {
      throw new IOException("autonomous-pair backlog record exceeds its size limit");
    }
    try {
      ByteArrayOutputStream encoded = new ByteArrayOutputStream((int) file.length());
      try (FileInputStream input = new FileInputStream(file)) {
        byte[] buffer = new byte[4 * 1024];
        int count;
        while ((count = input.read(buffer)) >= 0) {
          if (encoded.size() + count > PairedCoordinationRecord.MAXIMUM_SERIALIZED_BYTES) {
            throw new IOException("autonomous-pair backlog record exceeds its size limit");
          }
          encoded.write(buffer, 0, count);
        }
      }
      return PairedCoordinationRecord.fromJson(
          new String(encoded.toByteArray(), StandardCharsets.UTF_8));
    } catch (IllegalArgumentException malformed) {
      throw new IOException("autonomous-pair backlog record is malformed", malformed);
    }
  }

  private File checkpointFile() throws IOException {
    File file = new File(root, CHECKPOINT_FILE_NAME).getCanonicalFile();
    if (!file.getParentFile().equals(root)) {
      throw new IOException("autonomous-pair checkpoint escapes its app-private root");
    }
    return file;
  }

  private File backlogFile(String sharedSessionId) throws IOException {
    PairedCoordinationRecord.validateSharedSessionId(sharedSessionId);
    File file = new File(backlogRoot, sharedSessionId + ".json").getCanonicalFile();
    if (!file.getParentFile().equals(backlogRoot)) {
      throw new IOException("autonomous-pair backlog key escapes its app-private root");
    }
    return file;
  }

  private void ensureDirectory(File directory) throws IOException {
    requireSafeDirectoryOrMissing(directory, "autonomous-pair directory");
    if (directory.isDirectory()) {
      return;
    }
    if (!directory.mkdir()) {
      throw new IOException("unable to create autonomous-pair directory");
    }
    File parent = directory.getParentFile();
    if (parent != null) {
      directorySynchronizer.synchronize(parent);
    }
  }

  private static void requireSafeDirectoryOrMissing(File directory, String label)
      throws IOException {
    if (directory.exists()
        && (Files.isSymbolicLink(directory.toPath()) || !directory.isDirectory())) {
      throw new IOException(label + " is not a regular directory");
    }
  }

  private static void requireRegularFile(File file, String label) throws IOException {
    if (Files.isSymbolicLink(file.toPath()) || !file.isFile()) {
      throw new IOException(label + " is not a regular file");
    }
  }

  private static void replaceAtomically(File source, File target) throws IOException {
    try {
      Files.move(
          source.toPath(),
          target.toPath(),
          StandardCopyOption.ATOMIC_MOVE,
          StandardCopyOption.REPLACE_EXISTING);
    } catch (AtomicMoveNotSupportedException unsupported) {
      Files.move(source.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING);
    }
  }

  private static void writeString(DataOutputStream output, String value) throws IOException {
    byte[] encoded = value.getBytes(StandardCharsets.UTF_8);
    output.writeInt(encoded.length);
    output.write(encoded);
  }

  private static String readString(DataInputStream input, int maximumCharacters)
      throws IOException {
    int byteCount = input.readInt();
    int maximumBytes = Math.min(MAXIMUM_STRING_BYTES, maximumCharacters * 4);
    if (byteCount < 0 || byteCount > maximumBytes) {
      throw new IOException("autonomous-pair checkpoint string exceeds its bound");
    }
    byte[] encoded = new byte[byteCount];
    input.readFully(encoded);
    String value = new String(encoded, StandardCharsets.UTF_8);
    if (value.length() > maximumCharacters
        || !java.util.Arrays.equals(encoded, value.getBytes(StandardCharsets.UTF_8))) {
      throw new IOException("autonomous-pair checkpoint string is invalid UTF-8");
    }
    return value;
  }

  private static <T extends Enum<T>> T readEnum(
      DataInputStream input, Class<T> type, String label) throws IOException {
    String value = readString(input, 64);
    try {
      return Enum.valueOf(type, value);
    } catch (IllegalArgumentException unknown) {
      throw new IOException("unknown " + label + " in autonomous-pair checkpoint", unknown);
    }
  }
}
