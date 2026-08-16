package com.agoessling.swingcapture.core.coordination;

import java.io.IOException;
import java.util.Optional;

/**
 * Filesystem-free boundary for atomically publishing immutable coordination evidence.
 *
 * <p>A durable Android implementation should keep the revision and UTF-8 value in one atomically
 * replaced object. For a file implementation that means writing and syncing a temporary file,
 * atomically renaming it, and syncing the containing directory where the platform permits it.
 */
public interface AtomicTextStore {
  long MISSING_REVISION = 0;
  long CONFLICT = -1;

  /** One complete value observed at a single revision. Revisions start at one. */
  record VersionedText(long revision, String value) {
    public VersionedText {
      if (revision <= MISSING_REVISION) {
        throw new IllegalArgumentException("stored revision must be positive");
      }
      if (value == null) {
        throw new NullPointerException("value");
      }
    }
  }

  /** Reads one key without exposing a partially published value. */
  Optional<VersionedText> read(String key) throws IOException;

  /**
   * Atomically publishes {@code value} only if the current revision equals {@code expectedRevision}.
   * Use {@link #MISSING_REVISION} to create a previously absent key. Returns the new positive
   * revision on success or {@link #CONFLICT} when another value won the race.
   */
  long compareAndSet(String key, long expectedRevision, String value) throws IOException;
}
