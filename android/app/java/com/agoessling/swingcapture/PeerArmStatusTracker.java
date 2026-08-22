package com.agoessling.swingcapture;

import java.util.Objects;

/** Auditable, non-blocking outcome of coordinating a pose arm with the paired phone. */
final class PeerArmStatusTracker {
  enum State {
    NOT_REQUESTED,
    PENDING,
    ACCEPTED,
    REJECTED,
    FAILED,
    INBOUND_ACCEPTED
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

  synchronized void reset() {
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

  private static void requireSession(String sessionId) {
    if (sessionId == null || !sessionId.matches("[A-Za-z0-9._-]+")) {
      throw new IllegalArgumentException("shared session id is invalid");
    }
  }
}
