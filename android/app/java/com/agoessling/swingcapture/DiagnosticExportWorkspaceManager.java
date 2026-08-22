package com.agoessling.swingcapture;

import java.io.File;
import java.io.IOException;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.ArrayList;
import java.util.List;

/** Owns the single bounded, request-scoped diagnostic export workspace in app cache. */
final class DiagnosticExportWorkspaceManager {
  static final String ROOT_DIRECTORY_NAME = "diagnostic_exports";
  static final String WORKSPACE_PREFIX = "request-";

  private final File cacheDirectory;
  private Path activeWorkspace;

  DiagnosticExportWorkspaceManager(File cacheDirectory) {
    if (cacheDirectory == null) {
      throw new NullPointerException("cacheDirectory");
    }
    this.cacheDirectory = cacheDirectory;
  }

  /** Removes every workspace left by a terminated process without following links. */
  synchronized void cleanupStaleWorkspaces() throws IOException {
    if (activeWorkspace != null) {
      throw new IOException("cannot clean diagnostic exports while a workspace is active");
    }
    Path root = validatedRoot();
    List<Path> children = directChildren(root);
    for (Path child : children) {
      requireOwnedWorkspace(root, child);
    }
    for (Path child : children) {
      deleteEntryWithoutFollowingLinks(child);
    }
    if (!directChildren(root).isEmpty()) {
      throw new IOException("diagnostic export root is not empty after stale cleanup");
    }
  }

  /** Allocates the only active workspace; callers must release it in a finally block. */
  synchronized File createWorkspace() throws IOException {
    if (activeWorkspace != null) {
      throw new IOException("a diagnostic export workspace is already active");
    }
    Path root = validatedRoot();
    if (!directChildren(root).isEmpty()) {
      throw new IOException("stale diagnostic exports must be cleaned before allocation");
    }
    Path workspace = Files.createTempDirectory(root, WORKSPACE_PREFIX);
    requireOwnedWorkspace(root, workspace);
    activeWorkspace = workspace.toAbsolutePath().normalize();
    return workspace.toFile();
  }

  /** Releases one exact owned workspace, deleting links themselves rather than their targets. */
  synchronized void releaseWorkspace(File workspace) throws IOException {
    if (workspace == null) {
      throw new NullPointerException("workspace");
    }
    Path root = validatedRoot();
    Path requested = workspace.toPath().toAbsolutePath().normalize();
    requireOwnedWorkspace(root, requested);
    if (activeWorkspace == null || !activeWorkspace.equals(requested)) {
      throw new IOException("refusing to release a workspace other than the active export");
    }
    activeWorkspace = null;
    if (Files.exists(requested, LinkOption.NOFOLLOW_LINKS)) {
      deleteEntryWithoutFollowingLinks(requested);
    }
  }

  private Path validatedRoot() throws IOException {
    Path cache = cacheDirectory.toPath().toAbsolutePath().normalize();
    if (Files.isSymbolicLink(cache) || !Files.isDirectory(cache, LinkOption.NOFOLLOW_LINKS)) {
      throw new IOException("app cache must be a regular directory, not a link");
    }
    Path root = cache.resolve(ROOT_DIRECTORY_NAME);
    if (Files.exists(root, LinkOption.NOFOLLOW_LINKS)) {
      if (Files.isSymbolicLink(root)
          || !Files.isDirectory(root, LinkOption.NOFOLLOW_LINKS)) {
        throw new IOException("diagnostic export root must be a regular directory, not a link");
      }
    } else {
      Files.createDirectory(root);
    }
    if (!root.toRealPath().getParent().equals(cache.toRealPath())) {
      throw new IOException("diagnostic export root escapes the app cache");
    }
    return root.toAbsolutePath().normalize();
  }

  private static List<Path> directChildren(Path root) throws IOException {
    List<Path> children = new ArrayList<>();
    try (DirectoryStream<Path> stream = Files.newDirectoryStream(root)) {
      for (Path child : stream) {
        children.add(child.toAbsolutePath().normalize());
      }
    }
    return children;
  }

  private static void requireOwnedWorkspace(Path root, Path workspace) throws IOException {
    Path normalizedRoot = root.toAbsolutePath().normalize();
    Path normalizedWorkspace = workspace.toAbsolutePath().normalize();
    Path name = normalizedWorkspace.getFileName();
    if (!normalizedRoot.equals(normalizedWorkspace.getParent())
        || name == null
        || !name.toString().startsWith(WORKSPACE_PREFIX)) {
      throw new IOException("diagnostic export root contains an unexpected entry");
    }
  }

  private static void deleteEntryWithoutFollowingLinks(Path entry) throws IOException {
    BasicFileAttributes attributes =
        Files.readAttributes(
            entry,
            BasicFileAttributes.class,
            LinkOption.NOFOLLOW_LINKS);
    if (attributes.isDirectory()) {
      try (DirectoryStream<Path> stream = Files.newDirectoryStream(entry)) {
        for (Path child : stream) {
          deleteEntryWithoutFollowingLinks(child);
        }
      }
    } else if (!attributes.isRegularFile() && !attributes.isSymbolicLink()) {
      throw new IOException("diagnostic export workspace contains a special file");
    }
    Files.delete(entry);
  }
}
