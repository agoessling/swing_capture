package com.agoessling.swingcapture.core.coordination;

import java.io.IOException;
import java.util.Objects;
import java.util.Optional;

/** Validating, immutable-record repository over an atomic text persistence boundary. */
public final class CoordinationRecordStore {
  public enum StoreStatus {
    STORED,
    ALREADY_PRESENT,
    CONFLICT,
  }

  /** One validated record and the backing-store revision from the same atomic read. */
  public record VersionedRecord(long revision, PairedCoordinationRecord record) {
    public VersionedRecord {
      if (revision <= AtomicTextStore.MISSING_REVISION) {
        throw new IllegalArgumentException("record revision must be positive");
      }
      Objects.requireNonNull(record, "record");
    }
  }

  /** The winning stored value is returned for all outcomes, including a conflicting writer. */
  public record StoreResult(StoreStatus status, VersionedRecord stored) {
    public StoreResult {
      Objects.requireNonNull(status, "status");
      Objects.requireNonNull(stored, "stored");
    }
  }

  private final AtomicTextStore persistence;

  public CoordinationRecordStore(AtomicTextStore persistence) {
    this.persistence = Objects.requireNonNull(persistence, "persistence");
  }

  /** Reads and fully validates one paired record. */
  public Optional<VersionedRecord> read(String sharedSessionId) throws IOException {
    PairedCoordinationRecord.validateSharedSessionId(sharedSessionId);
    Optional<AtomicTextStore.VersionedText> stored = persistence.read(sharedSessionId);
    if (stored.isEmpty()) {
      return Optional.empty();
    }
    return Optional.of(decode(sharedSessionId, stored.orElseThrow()));
  }

  /**
   * Atomically creates an immutable record. Replaying byte-equivalent evidence is idempotent;
   * different evidence for the same shared session is reported as a conflict and never overwrites
   * the first accepted value.
   */
  public StoreResult storeIfAbsent(PairedCoordinationRecord record) throws IOException {
    Objects.requireNonNull(record, "record");
    String key = record.sharedSessionId();
    String encoded = record.toJson();

    Optional<VersionedRecord> existing = read(key);
    if (existing.isPresent()) {
      return resultForExisting(record, existing.orElseThrow());
    }

    long revision =
        persistence.compareAndSet(key, AtomicTextStore.MISSING_REVISION, encoded);
    if (revision != AtomicTextStore.CONFLICT) {
      if (revision <= AtomicTextStore.MISSING_REVISION) {
        throw new IOException("Atomic text store returned an invalid successful revision");
      }
      return new StoreResult(StoreStatus.STORED, new VersionedRecord(revision, record));
    }

    Optional<VersionedRecord> winner = read(key);
    if (winner.isEmpty()) {
      throw new IOException("Atomic text store reported a conflict without a winning value");
    }
    return resultForExisting(record, winner.orElseThrow());
  }

  private static StoreResult resultForExisting(
      PairedCoordinationRecord requested, VersionedRecord existing) {
    StoreStatus status =
        existing.record().equals(requested)
            ? StoreStatus.ALREADY_PRESENT
            : StoreStatus.CONFLICT;
    return new StoreResult(status, existing);
  }

  private static VersionedRecord decode(
      String expectedSharedSessionId, AtomicTextStore.VersionedText stored) throws IOException {
    final PairedCoordinationRecord record;
    try {
      record = PairedCoordinationRecord.fromJson(stored.value());
    } catch (IllegalArgumentException malformed) {
      throw new IOException("Stored coordination record is malformed", malformed);
    }
    if (!record.sharedSessionId().equals(expectedSharedSessionId)) {
      throw new IOException("Stored coordination record belongs to another shared session");
    }
    return new VersionedRecord(stored.revision(), record);
  }
}
