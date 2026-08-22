package com.agoessling.swingcapture;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

/** Host coverage for bounded, no-follow diagnostic export cache ownership. */
public final class DiagnosticExportWorkspaceManagerTest {
  private DiagnosticExportWorkspaceManagerTest() {}

  public static void main(String[] arguments) throws Exception {
    removesStaleWorkspaceTreesBeforeServingRequests();
    deletesLinksWithoutTouchingTheirTargets();
    rejectsLinkedOrUnexpectedRoots();
    permitsOnlyOneActiveWorkspaceAndOnlyExactRelease();
  }

  private static void removesStaleWorkspaceTreesBeforeServingRequests() throws Exception {
    Path cache = testDirectory("stale");
    Path root = Files.createDirectory(cache.resolve("diagnostic_exports"));
    Path first = Files.createDirectory(root.resolve("request-old-a"));
    Path nested = Files.createDirectories(first.resolve("session/pose_diagnostics"));
    Files.writeString(nested.resolve("trace.ndjson"), "evidence\n", StandardCharsets.UTF_8);
    Path second = Files.createDirectory(root.resolve("request-old-b"));
    Files.write(second.resolve("diagnostics.zip"), new byte[] {1, 2, 3});

    DiagnosticExportWorkspaceManager manager =
        new DiagnosticExportWorkspaceManager(cache.toFile());
    manager.cleanupStaleWorkspaces();

    check(Files.isDirectory(root), "dedicated export root remains available");
    check(isEmpty(root), "all stale request workspaces are removed");
  }

  private static void deletesLinksWithoutTouchingTheirTargets() throws Exception {
    Path cache = testDirectory("links");
    Path root = Files.createDirectory(cache.resolve("diagnostic_exports"));
    Path outside = Files.createDirectory(cache.resolve("outside"));
    Path evidence = Files.writeString(outside.resolve("keep.txt"), "keep", StandardCharsets.UTF_8);
    Path stale = Files.createDirectory(root.resolve("request-linked"));
    Files.createSymbolicLink(stale.resolve("external"), outside);

    DiagnosticExportWorkspaceManager manager =
        new DiagnosticExportWorkspaceManager(cache.toFile());
    manager.cleanupStaleWorkspaces();

    check(
        Files.readString(evidence, StandardCharsets.UTF_8).equals("keep"),
        "cleanup does not follow nested links");
    check(isEmpty(root), "workspace containing a link is removed");
  }

  private static void rejectsLinkedOrUnexpectedRoots() throws Exception {
    Path linkedCache = testDirectory("linked-root");
    Path outside = Files.createDirectory(linkedCache.resolve("outside"));
    Files.createSymbolicLink(linkedCache.resolve("diagnostic_exports"), outside);
    DiagnosticExportWorkspaceManager linked =
        new DiagnosticExportWorkspaceManager(linkedCache.toFile());
    expectIOException(linked::cleanupStaleWorkspaces, "linked export root");
    check(Files.isDirectory(outside), "linked root target remains untouched");

    Path unexpectedCache = testDirectory("unexpected-root");
    Path root = Files.createDirectory(unexpectedCache.resolve("diagnostic_exports"));
    Path unexpected = Files.writeString(root.resolve("unowned"), "keep", StandardCharsets.UTF_8);
    DiagnosticExportWorkspaceManager unexpectedManager =
        new DiagnosticExportWorkspaceManager(unexpectedCache.toFile());
    expectIOException(unexpectedManager::cleanupStaleWorkspaces, "unexpected root entry");
    check(Files.exists(unexpected), "unowned root entry is not destructively removed");
  }

  private static void permitsOnlyOneActiveWorkspaceAndOnlyExactRelease() throws Exception {
    Path cache = testDirectory("active");
    DiagnosticExportWorkspaceManager manager =
        new DiagnosticExportWorkspaceManager(cache.toFile());
    manager.cleanupStaleWorkspaces();
    File active = manager.createWorkspace();
    check(active.isDirectory(), "active workspace is allocated");
    expectIOException(manager::createWorkspace, "second active workspace");

    File other = new File(active.getParentFile(), "request-other");
    expectIOException(() -> manager.releaseWorkspace(other), "release a different workspace");
    check(active.isDirectory(), "wrong release preserves the active workspace");

    manager.releaseWorkspace(active);
    check(!active.exists(), "exact active workspace is removed");
    expectIOException(() -> manager.releaseWorkspace(active), "release an inactive workspace");
    File replacement = manager.createWorkspace();
    manager.releaseWorkspace(replacement);
  }

  private static Path testDirectory(String name) throws IOException {
    Path root = Path.of(System.getenv("TEST_TMPDIR"), "diagnostic-export-workspace-" + name);
    return Files.createDirectory(root);
  }

  private static boolean isEmpty(Path directory) throws IOException {
    try (var entries = Files.list(directory)) {
      return entries.findAny().isEmpty();
    }
  }

  private static void expectIOException(ThrowingAction action, String label) throws Exception {
    try {
      action.run();
      throw new AssertionError("expected IOException for " + label);
    } catch (IOException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }

  @FunctionalInterface
  private interface ThrowingAction {
    void run() throws Exception;
  }
}
