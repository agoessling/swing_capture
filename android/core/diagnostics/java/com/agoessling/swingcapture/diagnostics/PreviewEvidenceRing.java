package com.agoessling.swingcapture.diagnostics;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Objects;

/**
 * Duration-, count-, and byte-bounded flight recorder for 5 Hz pose observations and optional
 * lower-cadence compressed preview inputs.
 *
 * <p>The recommended initial field policy is 60 seconds, 300 observations, and 32 MiB. Construct
 * an instance explicitly with those constants; the class does not allocate a default ring.
 */
public final class PreviewEvidenceRing {
  public static final long RECOMMENDED_RETENTION_NANOS = 60_000_000_000L;
  public static final int RECOMMENDED_MAXIMUM_ENTRY_COUNT = 300;
  public static final long RECOMMENDED_MAXIMUM_COMPRESSED_BYTES = 32L * 1024 * 1024;

  public enum RejectionKind {
    NONMONOTONIC_TIMESTAMP,
    ENTRY_EXCEEDS_BYTE_CAPACITY,
  }

  /** A rejected append that leaves every retained entry unchanged. */
  public static final class AppendRejectedException extends IllegalArgumentException {
    private final RejectionKind kind;

    private AppendRejectedException(RejectionKind kind, String message) {
      super("preview append " + kind.name().toLowerCase(Locale.ROOT) + ": " + message);
      this.kind = kind;
    }

    public RejectionKind kind() {
      return kind;
    }
  }

  /** Detached entry ordering and immutable evidence objects for one half-open time range. */
  public static final class Snapshot {
    private final long firstTimestampInclusive;
    private final long endTimestampExclusive;
    private final List<PreviewEvidence> entries;
    private final long compressedBytes;

    private Snapshot(
        long firstTimestampInclusive,
        long endTimestampExclusive,
        List<PreviewEvidence> entries,
        long compressedBytes) {
      this.firstTimestampInclusive = firstTimestampInclusive;
      this.endTimestampExclusive = endTimestampExclusive;
      this.entries = List.copyOf(entries);
      this.compressedBytes = compressedBytes;
    }

    public long firstTimestampInclusive() {
      return firstTimestampInclusive;
    }

    public long endTimestampExclusive() {
      return endTimestampExclusive;
    }

    public int entryCount() {
      return entries.size();
    }

    public PreviewEvidence entryAt(int index) {
      return entries.get(index);
    }

    public List<PreviewEvidence> entries() {
      return entries;
    }

    public long compressedBytes() {
      return compressedBytes;
    }
  }

  private final long maximumDurationNanos;
  private final int maximumEntryCount;
  private final long maximumCompressedBytes;
  private final ArrayDeque<PreviewEvidence> entries = new ArrayDeque<>();
  private long compressedBytes;

  public PreviewEvidenceRing(
      long maximumDurationNanos, int maximumEntryCount, long maximumCompressedBytes) {
    if (maximumDurationNanos <= 0
        || maximumEntryCount <= 0
        || maximumCompressedBytes <= 0) {
      throw new IllegalArgumentException("preview duration, count, and byte bounds must be positive");
    }
    this.maximumDurationNanos = maximumDurationNanos;
    this.maximumEntryCount = maximumEntryCount;
    this.maximumCompressedBytes = maximumCompressedBytes;
  }

  /** Retains one immutable entry, evicting oldest entries until every configured bound holds. */
  public synchronized void append(PreviewEvidence evidence) {
    Objects.requireNonNull(evidence, "evidence");
    if (evidence.compressedFrameBytes() > maximumCompressedBytes) {
      throw new AppendRejectedException(
          RejectionKind.ENTRY_EXCEEDS_BYTE_CAPACITY,
          "entry has "
              + evidence.compressedFrameBytes()
              + " bytes but capacity is "
              + maximumCompressedBytes);
    }
    PreviewEvidence newest = entries.peekLast();
    if (newest != null
        && evidence.timestampBoottimeNanos() <= newest.timestampBoottimeNanos()) {
      throw new AppendRejectedException(
          RejectionKind.NONMONOTONIC_TIMESTAMP,
          "expected a timestamp after "
              + newest.timestampBoottimeNanos()
              + " but received "
              + evidence.timestampBoottimeNanos());
    }

    entries.addLast(evidence);
    compressedBytes = Math.addExact(compressedBytes, evidence.compressedFrameBytes());
    evictToBounds();
  }

  /** Attaches an asynchronously encoded frame to an already-retained observation. */
  public synchronized boolean attachCompressedFrame(long timestampBoottimeNanos, byte[] frame) {
    Objects.requireNonNull(frame, "frame");
    if (frame.length == 0 || frame.length > maximumCompressedBytes) {
      throw new AppendRejectedException(
          RejectionKind.ENTRY_EXCEEDS_BYTE_CAPACITY,
          "attached frame has " + frame.length + " bytes");
    }
    ArrayDeque<PreviewEvidence> replaced = new ArrayDeque<>(entries.size());
    boolean found = false;
    for (PreviewEvidence evidence : entries) {
      if (evidence.timestampBoottimeNanos() == timestampBoottimeNanos) {
        compressedBytes -= evidence.compressedFrameBytes();
        PreviewEvidence attached = evidence.withCompressedFrame(frame);
        compressedBytes = Math.addExact(compressedBytes, attached.compressedFrameBytes());
        replaced.addLast(attached);
        found = true;
      } else {
        replaced.addLast(evidence);
      }
    }
    if (!found) {
      return false;
    }
    entries.clear();
    entries.addAll(replaced);
    evictToBounds();
    return entries.stream()
        .anyMatch(evidence -> evidence.timestampBoottimeNanos() == timestampBoottimeNanos);
  }

  /** Returns every retained entry in timestamp order. */
  public synchronized Snapshot snapshot() {
    if (entries.isEmpty()) {
      return new Snapshot(0, 0, List.of(), 0);
    }
    long first = entries.getFirst().timestampBoottimeNanos();
    long last = entries.getLast().timestampBoottimeNanos();
    return snapshot(first, Math.addExact(last, 1));
  }

  /** Selects retained entries with timestamps in {@code [firstInclusive, endExclusive)}. */
  public synchronized Snapshot snapshot(long firstInclusive, long endExclusive) {
    if (firstInclusive < 0 || endExclusive <= firstInclusive) {
      throw new IllegalArgumentException("preview snapshot range must be nonnegative and increasing");
    }
    List<PreviewEvidence> selected = new ArrayList<>();
    long selectedBytes = 0;
    for (PreviewEvidence evidence : entries) {
      long timestamp = evidence.timestampBoottimeNanos();
      if (timestamp >= endExclusive) {
        break;
      }
      if (timestamp >= firstInclusive) {
        selected.add(evidence);
        selectedBytes = Math.addExact(selectedBytes, evidence.compressedFrameBytes());
      }
    }
    return new Snapshot(firstInclusive, endExclusive, selected, selectedBytes);
  }

  public synchronized int retainedEntryCount() {
    return entries.size();
  }

  public synchronized long retainedCompressedBytes() {
    return compressedBytes;
  }

  public long maximumDurationNanos() {
    return maximumDurationNanos;
  }

  public int maximumEntryCount() {
    return maximumEntryCount;
  }

  public long maximumCompressedBytes() {
    return maximumCompressedBytes;
  }

  private void evictToBounds() {
    while (entries.size() > maximumEntryCount
        || compressedBytes > maximumCompressedBytes
        || retainedDurationNanos() > maximumDurationNanos) {
      PreviewEvidence removed = entries.removeFirst();
      compressedBytes -= removed.compressedFrameBytes();
    }
  }

  private long retainedDurationNanos() {
    if (entries.size() < 2) {
      return 0;
    }
    return entries.getLast().timestampBoottimeNanos()
        - entries.getFirst().timestampBoottimeNanos();
  }
}
