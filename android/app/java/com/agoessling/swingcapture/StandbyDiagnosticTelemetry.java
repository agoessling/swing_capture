package com.agoessling.swingcapture;

import java.util.Objects;

/** Coherent process-local telemetry shared by standby callback, publisher, and shutdown threads. */
final class StandbyDiagnosticTelemetry {
  record Snapshot(
      long detectedEvents,
      long operatorTags,
      long publishedSessions,
      long droppedEvents,
      long timestampRejections,
      long discontinuities,
      long eventsCancelledForPoseArm,
      String lastError) {
    Snapshot {
      if (detectedEvents < 0
          || operatorTags < 0
          || publishedSessions < 0
          || droppedEvents < 0
          || timestampRejections < 0
          || discontinuities < 0
          || eventsCancelledForPoseArm < 0) {
        throw new IllegalArgumentException("standby diagnostic telemetry cannot be negative");
      }
      Objects.requireNonNull(lastError, "lastError");
    }
  }

  private long detectedEvents;
  private long operatorTags;
  private long publishedSessions;
  private long droppedEvents;
  private long timestampRejections;
  private long discontinuities;
  private long eventsCancelledForPoseArm;
  private String lastError = "";

  synchronized void recordDetectedEvent() {
    detectedEvents = Math.addExact(detectedEvents, 1L);
  }

  synchronized void recordOperatorTag() {
    operatorTags = Math.addExact(operatorTags, 1L);
  }

  synchronized void recordPublishedSession() {
    publishedSessions = Math.addExact(publishedSessions, 1L);
  }

  synchronized void addDroppedEvents(long count) {
    requireCount(count);
    droppedEvents = Math.addExact(droppedEvents, count);
  }

  /** Atomically accounts for dropped work and exposes the failure which caused it. */
  synchronized void recordDroppedEvents(long count, String error) {
    requireCount(count);
    droppedEvents = Math.addExact(droppedEvents, count);
    lastError = requireError(error);
  }

  synchronized void recordTimestampRejections(long rejectionCount) {
    requireCount(rejectionCount);
    timestampRejections = rejectionCount;
  }

  synchronized void recordDiscontinuity() {
    discontinuities = Math.addExact(discontinuities, 1L);
  }

  synchronized void addEventsCancelledForPoseArm(long count) {
    requireCount(count);
    eventsCancelledForPoseArm = Math.addExact(eventsCancelledForPoseArm, count);
  }

  synchronized void recordError(String error) {
    lastError = requireError(error);
  }

  synchronized void clearLastError() {
    lastError = "";
  }

  synchronized Snapshot snapshot() {
    return new Snapshot(
        detectedEvents,
        operatorTags,
        publishedSessions,
        droppedEvents,
        timestampRejections,
        discontinuities,
        eventsCancelledForPoseArm,
        lastError);
  }

  private static void requireCount(long count) {
    if (count < 0) {
      throw new IllegalArgumentException("standby diagnostic telemetry count cannot be negative");
    }
  }

  private static String requireError(String error) {
    if (error == null || error.isEmpty()) {
      throw new IllegalArgumentException("standby diagnostic telemetry error cannot be empty");
    }
    return error;
  }
}
