package com.agoessling.swingcapture;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Optional;

/** Bounded identity handoff from standby-audio event registration to publication. */
final class StandbyDiagnosticSessionRegistry {
  // The audio coordinator admits four live post-roll events. Allow four additional identities
  // whose frozen callbacks are queued but have not yet claimed their publisher handoff.
  static final int MAXIMUM_PENDING_SESSIONS = 8;

  record PendingSession(String sessionId, String sourceNodeId, long createdAtEpochMillis) {
    PendingSession {
      requireIdentifier(sessionId, "sessionId");
      requireIdentifier(sourceNodeId, "sourceNodeId");
      if (createdAtEpochMillis <= 0) {
        throw new IllegalArgumentException("createdAtEpochMillis must be positive");
      }
    }
  }

  private final Map<Long, PendingSession> sessions = new LinkedHashMap<>();

  synchronized void register(long eventSequence, PendingSession session) {
    if (eventSequence <= 0) {
      throw new IllegalArgumentException("eventSequence must be positive");
    }
    if (sessions.containsKey(eventSequence)) {
      throw new IllegalStateException("standby diagnostic event was registered twice");
    }
    if (sessions.size() >= MAXIMUM_PENDING_SESSIONS) {
      throw new IllegalStateException("standby diagnostic session registry is full");
    }
    sessions.put(eventSequence, session);
  }

  synchronized Optional<PendingSession> claim(long eventSequence) {
    return Optional.ofNullable(sessions.remove(eventSequence));
  }

  synchronized void discard(long eventSequence) {
    sessions.remove(eventSequence);
  }

  synchronized int size() {
    return sessions.size();
  }

  synchronized int clear() {
    int removed = sessions.size();
    sessions.clear();
    return removed;
  }

  private static void requireIdentifier(String value, String name) {
    if (value == null || !value.matches("[A-Za-z0-9._-]+") || value.length() > 128) {
      throw new IllegalArgumentException(name + " is invalid");
    }
  }
}
