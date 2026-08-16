package com.agoessling.swingcapture.core.coordination;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.Optional;

/**
 * One-shot, session-scoped pairing state machine. Reports are admitted as they arrive and one
 * deterministic decision is made at the explicit session deadline.
 */
public final class DualNodeSwingAssociator {
  /** Timing and evidence gates in the coordinator monotonic clock domain. */
  public record Config(
      long pairingToleranceNs,
      long maximumReportUncertaintyNs,
      long maximumPairUncertaintyNs,
      long maximumReportAgeNs) {
    public Config {
      if (pairingToleranceNs < 0
          || maximumReportUncertaintyNs < 0
          || maximumPairUncertaintyNs < 0
          || maximumReportAgeNs < 0) {
        throw new IllegalArgumentException("coordination timing limits cannot be negative");
      }
    }
  }

  private final String sessionId;
  private final long openedAtCoordinatorNs;
  private final long decisionDeadlineCoordinatorNs;
  private final Config config;
  private final List<ObservedNodeTrigger> accepted = new ArrayList<>();
  private AssociationResult terminalResult;

  public DualNodeSwingAssociator(
      String sessionId,
      long openedAtCoordinatorNs,
      long decisionDeadlineCoordinatorNs,
      Config config) {
    this.sessionId = Objects.requireNonNull(sessionId, "sessionId");
    this.config = Objects.requireNonNull(config, "config");
    if (sessionId.isBlank()) {
      throw new IllegalArgumentException("sessionId cannot be blank");
    }
    if (openedAtCoordinatorNs < 0 || decisionDeadlineCoordinatorNs < openedAtCoordinatorNs) {
      throw new IllegalArgumentException("session coordination window is invalid");
    }
    this.openedAtCoordinatorNs = openedAtCoordinatorNs;
    this.decisionDeadlineCoordinatorNs = decisionDeadlineCoordinatorNs;
  }

  public SubmissionResult submit(
      NodeTriggerReport report,
      ClockOffsetEstimate clockEstimate,
      long receivedAtCoordinatorNs) {
    Objects.requireNonNull(report, "report");
    if (terminalResult != null || receivedAtCoordinatorNs > decisionDeadlineCoordinatorNs) {
      return rejected(SubmissionStatus.LATE_REPORT, "report arrived after the decision deadline");
    }
    if (!sessionId.equals(report.sessionId())) {
      return rejected(SubmissionStatus.UNRELATED_SESSION, "report belongs to another session");
    }

    ObservedNodeTrigger observed =
        ObservedNodeTrigger.map(report, clockEstimate, receivedAtCoordinatorNs);
    if (observed.coordinatorUncertaintyNs() > config.maximumReportUncertaintyNs()) {
      return rejected(
          SubmissionStatus.EXCESSIVE_UNCERTAINTY,
          "mapped report uncertainty exceeds the per-report limit");
    }
    if (observed.latestCoordinatorTimestampNs() < openedAtCoordinatorNs
        || isOlderThanAllowed(observed)) {
      return rejected(SubmissionStatus.STALE_REPORT, "trigger predates the active session window");
    }
    if (observed.earliestCoordinatorTimestampNs() > decisionDeadlineCoordinatorNs) {
      return rejected(SubmissionStatus.LATE_REPORT, "trigger follows the active session window");
    }
    if (accepted.stream().anyMatch(candidate -> candidate.report().equals(report))) {
      return rejected(SubmissionStatus.DUPLICATE_REPORT, "the same node report was seen twice");
    }
    accepted.add(observed);
    return new SubmissionResult(SubmissionStatus.ACCEPTED, Optional.of(observed), "accepted");
  }

  public AssociationResult decide(long nowCoordinatorNs) {
    if (terminalResult != null) {
      return terminalResult;
    }
    if (nowCoordinatorNs < decisionDeadlineCoordinatorNs) {
      return result(AssociationStatus.NOT_READY, "decision deadline has not elapsed");
    }

    List<ObservedNodeTrigger> downTheLine = reportsFor(CaptureRole.DOWN_THE_LINE);
    List<ObservedNodeTrigger> faceOn = reportsFor(CaptureRole.FACE_ON);
    if (downTheLine.isEmpty() || faceOn.isEmpty()) {
      terminalResult =
          accepted.size() > 1
              ? result(AssociationStatus.DUPLICATE_ROLE, "multiple reports supplied one role only")
              : result(AssociationStatus.MISSING_OR_LATE, "one or both role reports are missing");
      return terminalResult;
    }
    if (downTheLine.size() != 1 || faceOn.size() != 1) {
      terminalResult = decideWithDuplicateRoles(downTheLine, faceOn);
      return terminalResult;
    }

    terminalResult = decidePair(downTheLine.get(0), faceOn.get(0));
    return terminalResult;
  }

  public int acceptedReportCount() {
    return accepted.size();
  }

  private boolean isOlderThanAllowed(ObservedNodeTrigger observed) {
    if (observed.receivedAtCoordinatorNs() <= observed.latestCoordinatorTimestampNs()) {
      return false;
    }
    long age =
        Math.subtractExact(
            observed.receivedAtCoordinatorNs(), observed.latestCoordinatorTimestampNs());
    return age > config.maximumReportAgeNs();
  }

  private List<ObservedNodeTrigger> reportsFor(CaptureRole role) {
    return accepted.stream().filter(report -> report.report().role() == role).toList();
  }

  private AssociationResult decideWithDuplicateRoles(
      List<ObservedNodeTrigger> downTheLine, List<ObservedNodeTrigger> faceOn) {
    int plausiblePairs = 0;
    for (ObservedNodeTrigger first : downTheLine) {
      for (ObservedNodeTrigger second : faceOn) {
        if (!first.report().nodeId().equals(second.report().nodeId())
            && minimumSeparation(first, second) <= config.pairingToleranceNs()) {
          ++plausiblePairs;
        }
      }
    }
    return plausiblePairs > 1
        ? result(AssociationStatus.AMBIGUOUS_TIMING, "multiple cross-role pairs are plausible")
        : result(AssociationStatus.DUPLICATE_ROLE, "a session contains repeated role reports");
  }

  private AssociationResult decidePair(
      ObservedNodeTrigger downTheLine, ObservedNodeTrigger faceOn) {
    if (downTheLine.report().nodeId().equals(faceOn.report().nodeId())) {
      return result(
          AssociationStatus.SAME_NODE_FOR_BOTH_ROLES,
          "one node cannot supply both roles in the same swing");
    }
    long combinedUncertainty =
        Math.addExact(
            downTheLine.coordinatorUncertaintyNs(), faceOn.coordinatorUncertaintyNs());
    if (combinedUncertainty > config.maximumPairUncertaintyNs()) {
      return result(
          AssociationStatus.EXCESSIVE_UNCERTAINTY,
          "combined trigger uncertainty exceeds the pair limit");
    }

    long minimumSeparation = minimumSeparation(downTheLine, faceOn);
    long maximumSeparation = maximumSeparation(downTheLine, faceOn);
    if (minimumSeparation > config.pairingToleranceNs()) {
      return result(
          AssociationStatus.UNRELATED_TRIGGERS,
          "trigger intervals are farther apart than the pairing tolerance");
    }
    if (maximumSeparation > config.pairingToleranceNs()) {
      return result(
          AssociationStatus.AMBIGUOUS_TIMING,
          "uncertainty cannot prove the triggers fall within the pairing tolerance");
    }

    AssociatedSwing swing =
        new AssociatedSwing(
            sessionId, downTheLine, faceOn, minimumSeparation, maximumSeparation);
    return new AssociationResult(AssociationStatus.PAIRED, Optional.of(swing), "paired");
  }

  private static long centerSeparation(ObservedNodeTrigger first, ObservedNodeTrigger second) {
    long delta =
        Math.subtractExact(first.coordinatorTimestampNs(), second.coordinatorTimestampNs());
    if (delta == Long.MIN_VALUE) {
      throw new ArithmeticException("trigger separation overflows long");
    }
    return Math.abs(delta);
  }

  private static long combinedUncertainty(
      ObservedNodeTrigger first, ObservedNodeTrigger second) {
    return Math.addExact(first.coordinatorUncertaintyNs(), second.coordinatorUncertaintyNs());
  }

  private static long minimumSeparation(
      ObservedNodeTrigger first, ObservedNodeTrigger second) {
    return Math.max(0, centerSeparation(first, second) - combinedUncertainty(first, second));
  }

  private static long maximumSeparation(
      ObservedNodeTrigger first, ObservedNodeTrigger second) {
    return Math.addExact(centerSeparation(first, second), combinedUncertainty(first, second));
  }

  private static SubmissionResult rejected(SubmissionStatus status, String detail) {
    return new SubmissionResult(status, Optional.empty(), detail);
  }

  private static AssociationResult result(AssociationStatus status, String detail) {
    return new AssociationResult(status, Optional.empty(), detail);
  }
}
