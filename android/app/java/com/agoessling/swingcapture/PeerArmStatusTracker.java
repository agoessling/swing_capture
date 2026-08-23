package com.agoessling.swingcapture;

import java.util.ArrayDeque;
import java.util.Objects;

/** Auditable, non-blocking outcome of coordinating a pose arm with the paired phone. */
final class PeerArmStatusTracker {
  private static final int MAXIMUM_RECENT_INBOUND_SESSIONS = 8;

  enum State {
    NOT_REQUESTED,
    PENDING,
    ACCEPTED,
    REJECTED,
    FAILED,
    INBOUND_ACCEPTED
  }

  enum InboundRequestDisposition {
    NEW,
    DUPLICATE_ACTIVE,
    STALE
  }

  record Snapshot(State state, String sharedSessionId, int httpStatus, String failureType) {
    Snapshot {
      Objects.requireNonNull(state, "state");
      Objects.requireNonNull(sharedSessionId, "sharedSessionId");
      Objects.requireNonNull(failureType, "failureType");
    }
  }

  private State state = State.NOT_REQUESTED;
  private String sharedSessionId = "";
  private int httpStatus;
  private String failureType = "";
  private final ArrayDeque<String> recentInboundSessions = new ArrayDeque<>();

  synchronized void reset() {
    if (state == State.INBOUND_ACCEPTED) {
      rememberInboundSession(sharedSessionId);
    }
    state = State.NOT_REQUESTED;
    sharedSessionId = "";
    httpStatus = 0;
    failureType = "";
  }

  synchronized void pending(String sessionId) {
    requireSession(sessionId);
    state = State.PENDING;
    sharedSessionId = sessionId;
    httpStatus = 0;
    failureType = "";
  }

  synchronized void inboundAccepted(String sessionId) {
    requireSession(sessionId);
    state = State.INBOUND_ACCEPTED;
    sharedSessionId = sessionId;
    httpStatus = 0;
    failureType = "";
  }

  synchronized void inboundFailed(String sessionId, Throwable failure) {
    Objects.requireNonNull(failure, "failure");
    if (state != State.INBOUND_ACCEPTED || !sharedSessionId.equals(sessionId)) {
      return;
    }
    rememberInboundSession(sessionId);
    state = State.FAILED;
    String type = failure.getClass().getName();
    failureType = type.substring(0, Math.min(type.length(), 160));
  }

  synchronized InboundRequestDisposition classifyInboundRequest(String sessionId) {
    requireSession(sessionId);
    if (state == State.INBOUND_ACCEPTED && sharedSessionId.equals(sessionId)) {
      return InboundRequestDisposition.DUPLICATE_ACTIVE;
    }
    return recentInboundSessions.contains(sessionId)
        ? InboundRequestDisposition.STALE
        : InboundRequestDisposition.NEW;
  }

  synchronized void response(String sessionId, boolean accepted, int statusCode) {
    if (!sharedSessionId.equals(sessionId) || state != State.PENDING) {
      return;
    }
    state = accepted ? State.ACCEPTED : State.REJECTED;
    httpStatus = statusCode;
  }

  synchronized void failed(String sessionId, Throwable failure) {
    Objects.requireNonNull(failure, "failure");
    if (!sharedSessionId.equals(sessionId) || state != State.PENDING) {
      return;
    }
    state = State.FAILED;
    String type = failure.getClass().getName();
    failureType = type.substring(0, Math.min(type.length(), 160));
  }

  synchronized Snapshot snapshot() {
    return new Snapshot(state, sharedSessionId, httpStatus, failureType);
  }

  private void rememberInboundSession(String sessionId) {
    if (sessionId.isEmpty() || recentInboundSessions.contains(sessionId)) {
      return;
    }
    if (recentInboundSessions.size() == MAXIMUM_RECENT_INBOUND_SESSIONS) {
      recentInboundSessions.removeFirst();
    }
    recentInboundSessions.addLast(sessionId);
  }

  private static void requireSession(String sessionId) {
    if (sessionId == null || !sessionId.matches("[A-Za-z0-9._-]+")) {
      throw new IllegalArgumentException("shared session id is invalid");
    }
  }
}
