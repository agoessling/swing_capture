package com.agoessling.swingcapture;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/** Strict cleanup for app-owned, unpublished session staging directories. */
final class SessionStagingCleanup {
  static final String OWNERSHIP_MARKER = ".swing-capture-staging-v1";
  static final long DEFAULT_STALE_AGE_MILLIS = 60L * 60L * 1_000L;

  private SessionStagingCleanup() {}

  static void markOwned(File sessionsRoot, File stagingDirectory) throws IOException {
    validateDirectStagingDirectory(sessionsRoot, stagingDirectory, false);
    File marker = new File(stagingDirectory, OWNERSHIP_MARKER);
    if (marker.exists() || Files.isSymbolicLink(marker.toPath())) {
      throw new IOException("session staging ownership marker already exists");
    }
    try (FileOutputStream output = new FileOutputStream(marker)) {
      output.write("swing-capture\n".getBytes(StandardCharsets.US_ASCII));
      output.getFD().sync();
    }
  }

  static boolean cleanupFailedPublication(File sessionsRoot, File stagingDirectory) {
    try {
      validateDirectStagingDirectory(sessionsRoot, stagingDirectory, true);
      validateTree(stagingDirectory, stagingDirectory.getCanonicalFile());
      deleteTree(stagingDirectory);
      return true;
    } catch (IOException unsafeOrChanged) {
      return false;
    }
  }

  /** Removes only a just-created empty directory (or its sole regular marker) after mark failure. */
  static boolean cleanupAfterMarkFailure(File sessionsRoot, File stagingDirectory) {
    try {
      validateDirectStagingDirectory(sessionsRoot, stagingDirectory, false);
      File[] children = stagingDirectory.listFiles();
      if (children == null || children.length > 1) {
        return false;
      }
      if (children.length == 1
          && (!children[0].getName().equals(OWNERSHIP_MARKER)
              || !children[0].isFile()
              || Files.isSymbolicLink(children[0].toPath()))) {
        return false;
      }
      if (children.length == 1) {
        Files.delete(children[0].toPath());
      }
      Files.delete(stagingDirectory.toPath());
      return true;
    } catch (IOException unsafeOrChanged) {
      return false;
    }
  }

  static List<String> cleanupStale(
      File sessionsRoot, long nowEpochMillis, long minimumAgeMillis) throws IOException {
    if (nowEpochMillis < 0 || minimumAgeMillis < 0) {
      throw new IllegalArgumentException("cleanup times cannot be negative");
    }
    if (!sessionsRoot.isDirectory() || Files.isSymbolicLink(sessionsRoot.toPath())) {
      return List.of();
    }
    File canonicalRoot = sessionsRoot.getCanonicalFile();
    File[] children = sessionsRoot.listFiles();
    if (children == null) {
      throw new IOException("Unable to list session staging storage");
    }
    List<String> deleted = new ArrayList<>();
    for (File candidate : children) {
      if (!candidate.getName().matches("[A-Za-z0-9][A-Za-z0-9._-]*\\.tmp")) {
        continue;
      }
      long modified = candidate.lastModified();
      if (modified <= 0 || modified > nowEpochMillis
          || nowEpochMillis - modified < minimumAgeMillis) {
        continue;
      }
      try {
        validateDirectStagingDirectory(canonicalRoot, candidate, true);
        File canonicalCandidate = candidate.getCanonicalFile();
        validateTree(candidate, canonicalCandidate);
        deleteTree(candidate);
        deleted.add(candidate.getName());
      } catch (IOException unsafeOrChanged) {
        // Unknown, linked, or concurrently changed paths are never cleanup targets.
      }
    }
    return List.copyOf(deleted);
  }

  private static void validateDirectStagingDirectory(
      File sessionsRoot, File stagingDirectory, boolean requireMarker) throws IOException {
    if (!sessionsRoot.isDirectory()
        || Files.isSymbolicLink(sessionsRoot.toPath())
        || !stagingDirectory.isDirectory()
        || Files.isSymbolicLink(stagingDirectory.toPath())
        || !stagingDirectory.getName().matches("[A-Za-z0-9][A-Za-z0-9._-]*\\.tmp")
        || !stagingDirectory
            .getCanonicalFile()
            .getParentFile()
            .equals(sessionsRoot.getCanonicalFile())) {
      throw new IOException("Refusing unsafe session staging directory");
    }
    if (requireMarker) {
      File marker = new File(stagingDirectory, OWNERSHIP_MARKER);
      if (!marker.isFile()
          || Files.isSymbolicLink(marker.toPath())
          || !marker.getCanonicalFile().getParentFile().equals(stagingDirectory.getCanonicalFile())) {
        throw new IOException("Session staging directory is not app-owned");
      }
    }
  }

  private static void validateTree(File entry, File canonicalRoot) throws IOException {
    if (Files.isSymbolicLink(entry.toPath())) {
      throw new IOException("Refusing symbolic link in session staging tree");
    }
    File canonical = entry.getCanonicalFile();
    if (!canonical.equals(canonicalRoot)
        && !canonical.toPath().startsWith(canonicalRoot.toPath())) {
      throw new IOException("Session staging entry escapes its root");
    }
    if (!entry.getName().matches("[A-Za-z0-9._-]+")) {
      throw new IOException("Unsafe session staging entry name");
    }
    if (entry.isDirectory()) {
      File[] children = entry.listFiles();
      if (children == null) {
        throw new IOException("Unable to inspect session staging tree");
      }
      for (File child : children) {
        validateTree(child, canonicalRoot);
      }
    } else if (!entry.isFile()) {
      throw new IOException("Unsupported session staging entry type");
    }
  }

  private static void deleteTree(File entry) throws IOException {
    if (Files.isSymbolicLink(entry.toPath())) {
      throw new IOException("Session staging path changed before cleanup");
    }
    if (entry.isDirectory()) {
      File[] children = entry.listFiles();
      if (children == null) {
        throw new IOException("Unable to inspect session staging cleanup target");
      }
      for (File child : children) {
        deleteTree(child);
      }
    }
    Files.delete(entry.toPath());
  }
}
