package com.agoessling.swingcapture;

import com.agoessling.swingcapture.core.coordination.AtomicTextStore;
import com.agoessling.swingcapture.core.coordination.PairedCoordinationRecord;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;

/** Hermetic durability, publication, and immutability tests for the app-private file adapter. */
public final class CoordinationFileTextStoreTest {
  private CoordinationFileTextStoreTest() {}

  public static void main(String[] arguments) throws Exception {
    createReadAndConflictAreImmutable();
    adaptersForTheSameRootSerializeCompetingCreators();
    invalidOrCorruptFilesystemValuesAreRejected();
  }

  private static void createReadAndConflictAreImmutable() throws Exception {
    Path base = createTestDirectory("coordination-file-store-test-");
    try {
      Path root = base.resolve("coordination");
      List<Path> synchronizedDirectories = new ArrayList<>();
      CoordinationFileTextStore store =
          new CoordinationFileTextStore(
              root.toFile(), directory -> synchronizedDirectories.add(directory.toPath()));

      check(store.read("s").isEmpty(), "missing value");
      check(
          store.compareAndSet("s", AtomicTextStore.MISSING_REVISION, "first") == 1,
          "first value stored");
      AtomicTextStore.VersionedText stored = store.read("s").orElseThrow();
      check(stored.revision() == 1, "implicit immutable revision");
      check(stored.value().equals("first"), "stored bytes round trip");
      check(Files.readString(root.resolve("s.json")).equals("first"), "canonical file name");
      check(
          store.compareAndSet("s", AtomicTextStore.MISSING_REVISION, "second")
              == AtomicTextStore.CONFLICT,
          "missing-revision replay conflicts");
      check(
          store.compareAndSet("s", 1, "replacement") == AtomicTextStore.CONFLICT,
          "immutable value cannot be replaced");
      check(store.read("s").orElseThrow().value().equals("first"), "winner is unchanged");
      check(
          synchronizedDirectories.equals(List.of(base, root)),
          "new directory and published entry are synchronized");
      try (var entries = Files.list(root)) {
        check(entries.noneMatch(path -> path.getFileName().toString().endsWith(".tmp")), "no temp");
      }
    } finally {
      deleteTree(base);
    }
  }

  private static void adaptersForTheSameRootSerializeCompetingCreators() throws Exception {
    Path base = createTestDirectory("coordination-file-race-test-");
    try {
      Path root = base.resolve("coordination");
      CoordinationFileTextStore first = new CoordinationFileTextStore(root.toFile(), ignored -> {});
      CoordinationFileTextStore second =
          new CoordinationFileTextStore(root.toFile(), ignored -> {});
      CountDownLatch start = new CountDownLatch(1);
      AtomicInteger stored = new AtomicInteger();
      AtomicInteger conflicts = new AtomicInteger();
      List<Throwable> failures = new ArrayList<>();
      Thread firstWriter = writer(first, "first", start, stored, conflicts, failures);
      Thread secondWriter = writer(second, "second", start, stored, conflicts, failures);
      firstWriter.start();
      secondWriter.start();
      start.countDown();
      firstWriter.join();
      secondWriter.join();

      check(failures.isEmpty(), "competing writes complete without exceptions");
      check(stored.get() == 1, "exactly one creator wins");
      check(conflicts.get() == 1, "exactly one creator conflicts");
      String winner = first.read("shared").orElseThrow().value();
      check(winner.equals("first") || winner.equals("second"), "complete winner is visible");
    } finally {
      deleteTree(base);
    }
  }

  private static void invalidOrCorruptFilesystemValuesAreRejected() throws Exception {
    Path base = createTestDirectory("coordination-file-invalid-test-");
    try {
      CoordinationFileTextStore store =
          new CoordinationFileTextStore(base.resolve("coordination").toFile(), ignored -> {});
      expectIllegalArgument(() -> store.read("bad/session"), "unsafe key");
      expectIOException(
          () ->
              store.compareAndSet(
                  "shared",
                  AtomicTextStore.MISSING_REVISION,
                  "x".repeat(PairedCoordinationRecord.MAXIMUM_SERIALIZED_BYTES + 1)),
          "oversized write");

      Path root = base.resolve("coordination");
      Files.createDirectory(root);
      Files.write(
          root.resolve("oversized.json"),
          "x".repeat(PairedCoordinationRecord.MAXIMUM_SERIALIZED_BYTES + 1)
              .getBytes(StandardCharsets.UTF_8));
      expectIOException(() -> store.read("oversized"), "oversized stored value");

      Path outside = base.resolve("outside.json");
      Files.writeString(outside, "outside");
      Files.createSymbolicLink(root.resolve("linked.json"), outside);
      expectIOException(() -> store.read("linked"), "symbolic-link record");
    } finally {
      deleteTree(base);
    }
  }

  private static Thread writer(
      CoordinationFileTextStore store,
      String value,
      CountDownLatch start,
      AtomicInteger stored,
      AtomicInteger conflicts,
      List<Throwable> failures) {
    return new Thread(
        () -> {
          try {
            start.await();
            long result =
                store.compareAndSet("shared", AtomicTextStore.MISSING_REVISION, value);
            if (result == 1) {
              stored.incrementAndGet();
            } else if (result == AtomicTextStore.CONFLICT) {
              conflicts.incrementAndGet();
            } else {
              failures.add(new AssertionError("unexpected result " + result));
            }
          } catch (Throwable failure) {
            synchronized (failures) {
              failures.add(failure);
            }
          }
        });
  }

  private static Path createTestDirectory(String prefix) throws IOException {
    String testTemporaryDirectory = System.getenv("TEST_TMPDIR");
    if (testTemporaryDirectory == null || testTemporaryDirectory.isEmpty()) {
      throw new IOException("Bazel TEST_TMPDIR is required");
    }
    return Files.createTempDirectory(Path.of(testTemporaryDirectory), prefix);
  }

  private static void deleteTree(Path root) throws IOException {
    try (var paths = Files.walk(root)) {
      for (Path path : paths.sorted(Comparator.reverseOrder()).toList()) {
        Files.deleteIfExists(path);
      }
    }
  }

  private static void expectIllegalArgument(ThrowingAction action, String label) {
    try {
      action.run();
      throw new AssertionError("Expected invalid value: " + label);
    } catch (IllegalArgumentException expected) {
      // Expected.
    } catch (Exception unexpected) {
      throw new AssertionError("Unexpected exception: " + label, unexpected);
    }
  }

  private static void expectIOException(ThrowingAction action, String label) {
    try {
      action.run();
      throw new AssertionError("Expected I/O failure: " + label);
    } catch (IOException expected) {
      // Expected.
    } catch (Exception unexpected) {
      throw new AssertionError("Unexpected exception: " + label, unexpected);
    }
  }

  private static void check(boolean condition, String label) {
    if (!condition) {
      throw new AssertionError(label);
    }
  }

  @FunctionalInterface
  private interface ThrowingAction {
    void run() throws Exception;
  }
}
