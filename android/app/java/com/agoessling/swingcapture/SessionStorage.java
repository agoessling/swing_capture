package com.agoessling.swingcapture;

import android.content.Context;
import com.agoessling.swingcapture.node.SessionRetentionPlanner;
import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;

/** Validated app-private session expiry after successful atomic publication. */
public final class SessionStorage {
  private static final int MAXIMUM_SESSIONS = 40;
  private static final long MAXIMUM_SESSION_BYTES = 6L * 1024 * 1024 * 1024;
  private static final long MINIMUM_FREE_BYTES = 2L * 1024 * 1024 * 1024;

  private SessionStorage() {}

  public static List<String> enforceRetention(Context context, Set<String> protectedIds)
      throws IOException {
    File root = new File(context.getFilesDir(), "sessions");
    if (!root.isDirectory()) {
      return List.of();
    }
    List<SessionRetentionPlanner.Entry> entries = new ArrayList<>();
    long totalBytes = 0;
    File[] children = root.listFiles();
    if (children == null) {
      throw new IOException("Unable to list session storage");
    }
    for (File child : children) {
      if (!isPublishedSessionDirectory(root, child)) {
        continue;
      }
      long bytes = treeBytes(child);
      totalBytes = saturatedAdd(totalBytes, bytes);
      entries.add(
          new SessionRetentionPlanner.Entry(child.getName(), child.lastModified(), bytes));
    }
    long freeDeficit = Math.max(0, MINIMUM_FREE_BYTES - root.getUsableSpace());
    long spaceBudget = Math.max(1, totalBytes - freeDeficit);
    long effectiveMaximumBytes = Math.min(MAXIMUM_SESSION_BYTES, spaceBudget);
    List<String> deletions =
        SessionRetentionPlanner.deletions(
            entries, MAXIMUM_SESSIONS, effectiveMaximumBytes, protectedIds);
    List<String> deleted = new ArrayList<>();
    for (String sessionId : deletions) {
      File target = new File(root, sessionId);
      if (!isPublishedSessionDirectory(root, target)) {
        throw new IOException("Retention target changed during validation: " + sessionId);
      }
      deleteTree(target);
      deleted.add(sessionId);
    }
    return List.copyOf(deleted);
  }

  private static boolean isPublishedSessionDirectory(File root, File candidate)
      throws IOException {
    return candidate.isDirectory()
        && !candidate.getName().endsWith(".tmp")
        && candidate.getName().matches("[A-Za-z0-9._-]+")
        && !Files.isSymbolicLink(candidate.toPath())
        && candidate.getCanonicalFile().getParentFile().equals(root.getCanonicalFile())
        && new File(candidate, "manifest.json").isFile();
  }

  private static long treeBytes(File directory) throws IOException {
    if (Files.isSymbolicLink(directory.toPath())) {
      throw new IOException("Refusing to traverse a symbolic link in session storage");
    }
    if (directory.isFile()) {
      return directory.length();
    }
    File[] children = directory.listFiles();
    if (children == null) {
      throw new IOException("Unable to inspect " + directory);
    }
    long total = 0;
    for (File child : children) {
      total = saturatedAdd(total, treeBytes(child));
    }
    return total;
  }

  private static void deleteTree(File target) throws IOException {
    if (Files.isSymbolicLink(target.toPath())) {
      throw new IOException("Refusing to delete a symbolic link in session storage");
    }
    if (target.isDirectory()) {
      File[] children = target.listFiles();
      if (children == null) {
        throw new IOException("Unable to inspect retention target " + target);
      }
      for (File child : children) {
        deleteTree(child);
      }
    }
    Files.delete(target.toPath());
  }

  private static long saturatedAdd(long left, long right) {
    return Long.MAX_VALUE - left < right ? Long.MAX_VALUE : left + right;
  }
}
