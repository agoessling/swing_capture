package com.agoessling.swingcapture;

import java.io.File;
import java.nio.file.Files;
import java.util.List;

/** Deterministic interrupted-publication and restart cleanup coverage. */
public final class SessionStagingCleanupTest {
  private static final long NOW = 10_000_000L;

  private SessionStagingCleanupTest() {}

  public static void main(String[] arguments) throws Exception {
    staleOwnedStagingIsRemovedButFreshAndUnknownDataSurvive();
    failedPublicationIsCleanedImmediately();
    symbolicLinksAreNeverTraversed();
    markFailureCleanupOnlyRemovesEmptyNewStaging();
  }

  private static void staleOwnedStagingIsRemovedButFreshAndUnknownDataSurvive() throws Exception {
    File root = Files.createTempDirectory("session-staging-restart-").toFile();
    File stale = owned(root, "stale.tmp");
    check(new File(stale, "nested").mkdir(), "nested directory");
    Files.writeString(new File(stale, "nested/frame.bin").toPath(), "frame");
    check(stale.setLastModified(1_000), "stale timestamp");
    File fresh = owned(root, "fresh.tmp");
    check(fresh.setLastModified(NOW - 50), "fresh timestamp");
    File unknown = new File(root, "unknown.tmp");
    check(unknown.mkdir(), "unknown staging");
    check(unknown.setLastModified(1_000), "unknown timestamp");
    File malformed = owned(root, "valid.tmp");
    Files.writeString(new File(malformed, "unsafe name").toPath(), "data");
    check(malformed.setLastModified(1_000), "malformed timestamp");

    List<String> removed = SessionStagingCleanup.cleanupStale(root, NOW, 1_000);
    check(removed.equals(List.of("stale.tmp")), "only validated stale staging removed");
    check(!stale.exists(), "stale removed");
    check(fresh.exists(), "fresh survives");
    check(unknown.exists(), "unowned survives");
    check(malformed.exists(), "malformed owned tree survives");
  }

  private static void failedPublicationIsCleanedImmediately() throws Exception {
    File root = Files.createTempDirectory("session-staging-failure-").toFile();
    File staging = owned(root, "failure.tmp");
    Files.writeString(new File(staging, "manifest.json.tmp").toPath(), "partial");
    check(
        SessionStagingCleanup.cleanupFailedPublication(root, staging),
        "validated failed staging cleanup succeeds");
    check(!staging.exists(), "failed staging removed");
  }

  private static void symbolicLinksAreNeverTraversed() throws Exception {
    File root = Files.createTempDirectory("session-staging-link-").toFile();
    File outside = Files.createTempDirectory("session-staging-outside-").toFile();
    File sentinel = new File(outside, "sentinel");
    Files.writeString(sentinel.toPath(), "keep");
    File staging = owned(root, "linked.tmp");
    Files.createSymbolicLink(new File(staging, "escape").toPath(), outside.toPath());
    check(staging.setLastModified(1_000), "link timestamp");

    check(
        SessionStagingCleanup.cleanupStale(root, NOW, 1_000).isEmpty(),
        "linked staging is skipped");
    check(staging.exists(), "linked staging survives for manual inspection");
    check(sentinel.isFile(), "outside target remains untouched");
  }

  private static void markFailureCleanupOnlyRemovesEmptyNewStaging() throws Exception {
    File root = Files.createTempDirectory("session-staging-mark-").toFile();
    File empty = new File(root, "empty.tmp");
    check(empty.mkdir(), "empty mark failure staging");
    check(
        SessionStagingCleanup.cleanupAfterMarkFailure(root, empty),
        "empty mark failure is recoverable");
    File changed = new File(root, "changed.tmp");
    check(changed.mkdir(), "changed mark failure staging");
    Files.writeString(new File(changed, "unexpected").toPath(), "keep");
    check(
        !SessionStagingCleanup.cleanupAfterMarkFailure(root, changed),
        "changed unowned staging is preserved");
    check(changed.exists(), "changed mark staging remains");
  }

  private static File owned(File root, String name) throws Exception {
    File staging = new File(root, name);
    check(staging.mkdir(), "create " + name);
    SessionStagingCleanup.markOwned(root, staging);
    return staging;
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
