package com.agoessling.swingcapture;

import com.agoessling.swingcapture.core.coordination.AtomicTextStore;
import com.agoessling.swingcapture.core.coordination.PairedCoordinationRecord;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Optional;
import java.util.concurrent.ConcurrentHashMap;

/** App-private, create-only storage for immutable coordination records. */
public final class CoordinationFileTextStore implements AtomicTextStore {
  /** Platform hook used to make a published directory entry durable where supported. */
  @FunctionalInterface
  public interface DirectorySynchronizer {
    void synchronize(File directory) throws IOException;
  }

  private static final ConcurrentHashMap<String, Object> ROOT_LOCKS = new ConcurrentHashMap<>();
  private static final int REVISION = 1;

  private final File root;
  private final Object rootLock;
  private final DirectorySynchronizer directorySynchronizer;

  public CoordinationFileTextStore(File root, DirectorySynchronizer directorySynchronizer)
      throws IOException {
    if (root == null) {
      throw new NullPointerException("root");
    }
    if (directorySynchronizer == null) {
      throw new NullPointerException("directorySynchronizer");
    }
    if (Files.isSymbolicLink(root.toPath())) {
      throw new IOException("Coordination storage root must not be a symbolic link");
    }
    this.root = root.getCanonicalFile();
    this.rootLock = ROOT_LOCKS.computeIfAbsent(this.root.getPath(), ignored -> new Object());
    this.directorySynchronizer = directorySynchronizer;
  }

  @Override
  public Optional<VersionedText> read(String key) throws IOException {
    validateKey(key);
    synchronized (rootLock) {
      return readLocked(key);
    }
  }

  @Override
  public long compareAndSet(String key, long expectedRevision, String value) throws IOException {
    validateKey(key);
    if (expectedRevision < MISSING_REVISION) {
      throw new IllegalArgumentException("expectedRevision must not be negative");
    }
    if (value == null) {
      throw new NullPointerException("value");
    }
    byte[] encoded = value.getBytes(StandardCharsets.UTF_8);
    if (encoded.length > PairedCoordinationRecord.MAXIMUM_SERIALIZED_BYTES) {
      throw new IOException("Coordination value exceeds the serialized size limit");
    }

    synchronized (rootLock) {
      Optional<VersionedText> current = readLocked(key);
      long currentRevision = current.isPresent() ? REVISION : MISSING_REVISION;
      if (currentRevision != expectedRevision || currentRevision != MISSING_REVISION) {
        return CONFLICT;
      }

      ensureRootLocked();
      File target = targetFor(key);
      File temporary = File.createTempFile("cr-" + key + "-", ".tmp", root);
      try {
        try (FileOutputStream output = new FileOutputStream(temporary)) {
          output.write(encoded);
          output.flush();
          output.getFD().sync();
        }
        if (target.exists()) {
          return CONFLICT;
        }
        try {
          Files.move(temporary.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE);
        } catch (AtomicMoveNotSupportedException unsupported) {
          Files.move(temporary.toPath(), target.toPath());
        }
        directorySynchronizer.synchronize(root);
        return REVISION;
      } finally {
        Files.deleteIfExists(temporary.toPath());
      }
    }
  }

  private Optional<VersionedText> readLocked(String key) throws IOException {
    ensureExistingRootIsSafe();
    File target = targetFor(key);
    if (!target.exists()) {
      return Optional.empty();
    }
    if (Files.isSymbolicLink(target.toPath()) || !target.isFile()) {
      throw new IOException("Coordination record is not a regular app-private file");
    }

    try (FileInputStream input = new FileInputStream(target);
        ByteArrayOutputStream output = new ByteArrayOutputStream()) {
      byte[] buffer = new byte[4 * 1024];
      int count;
      while ((count = input.read(buffer)) >= 0) {
        if (output.size() + count > PairedCoordinationRecord.MAXIMUM_SERIALIZED_BYTES) {
          throw new IOException("Stored coordination value exceeds the serialized size limit");
        }
        output.write(buffer, 0, count);
      }
      return Optional.of(
          new VersionedText(REVISION, new String(output.toByteArray(), StandardCharsets.UTF_8)));
    }
  }

  private void ensureRootLocked() throws IOException {
    ensureExistingRootIsSafe();
    if (root.isDirectory()) {
      return;
    }
    if (!root.mkdir()) {
      throw new IOException("Unable to create the app-private coordination directory");
    }
    File parent = root.getParentFile();
    if (parent != null) {
      directorySynchronizer.synchronize(parent);
    }
  }

  private void ensureExistingRootIsSafe() throws IOException {
    if (!root.exists()) {
      return;
    }
    if (Files.isSymbolicLink(root.toPath()) || !root.isDirectory()) {
      throw new IOException("Coordination storage root is not a regular directory");
    }
  }

  private File targetFor(String key) throws IOException {
    File target = new File(root, key + ".json");
    if (!root.equals(target.getCanonicalFile().getParentFile())) {
      throw new IOException("Coordination storage key escapes its app-private directory");
    }
    return target;
  }

  private static void validateKey(String key) {
    PairedCoordinationRecord.validateSharedSessionId(key);
  }
}
