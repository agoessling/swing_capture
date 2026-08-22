package com.agoessling.swingcapture.node;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/** Pure retention policy. The Android storage layer performs the validated deletions. */
public final class SessionRetentionPlanner {
  public enum RetentionClass {
    PRIMARY_CAPTURE,
    DIAGNOSTIC
  }

  public static final class Entry {
    private final String sessionId;
    private final long createdAtEpochMillis;
    private final long bytes;
    private final RetentionClass retentionClass;

    public Entry(String sessionId, long createdAtEpochMillis, long bytes) {
      this(sessionId, createdAtEpochMillis, bytes, RetentionClass.PRIMARY_CAPTURE);
    }

    public Entry(
        String sessionId,
        long createdAtEpochMillis,
        long bytes,
        RetentionClass retentionClass) {
      if (sessionId == null || !sessionId.matches("[A-Za-z0-9._-]+")) {
        throw new IllegalArgumentException("invalid session id");
      }
      if (createdAtEpochMillis < 0 || bytes < 0) {
        throw new IllegalArgumentException("negative session metadata");
      }
      this.sessionId = sessionId;
      this.createdAtEpochMillis = createdAtEpochMillis;
      this.bytes = bytes;
      this.retentionClass = java.util.Objects.requireNonNull(retentionClass, "retentionClass");
    }

    public String sessionId() {
      return sessionId;
    }

    public long createdAtEpochMillis() {
      return createdAtEpochMillis;
    }

    public long bytes() {
      return bytes;
    }

    public RetentionClass retentionClass() {
      return retentionClass;
    }
  }

  private SessionRetentionPlanner() {}

  public static List<String> deletions(
      List<Entry> entries, int maximumSessions, long maximumBytes, Set<String> protectedIds) {
    if (maximumSessions < 1 || maximumBytes < 1) {
      throw new IllegalArgumentException("retention limits must be positive");
    }
    Set<String> protectedSet = protectedIds == null ? Set.of() : new HashSet<>(protectedIds);
    List<Entry> newestFirst = new ArrayList<>(entries);
    newestFirst.sort(
        Comparator.comparingInt(
                (Entry entry) ->
                    entry.retentionClass() == RetentionClass.PRIMARY_CAPTURE ? 0 : 1)
            .thenComparing(Comparator.comparingLong(Entry::createdAtEpochMillis).reversed())
            .thenComparing(Entry::sessionId));

    long keptBytes = 0;
    int keptSessions = 0;
    List<String> removed = new ArrayList<>();
    for (Entry entry : newestFirst) {
      if (protectedSet.contains(entry.sessionId())) {
        keptSessions++;
        keptBytes = saturatedAdd(keptBytes, entry.bytes());
        continue;
      }
      if (keptSessions < maximumSessions && entry.bytes() <= maximumBytes - keptBytes) {
        keptSessions++;
        keptBytes += entry.bytes();
      } else {
        removed.add(entry.sessionId());
      }
    }
    return List.copyOf(removed);
  }

  private static long saturatedAdd(long left, long right) {
    return Long.MAX_VALUE - left < right ? Long.MAX_VALUE : left + right;
  }
}
