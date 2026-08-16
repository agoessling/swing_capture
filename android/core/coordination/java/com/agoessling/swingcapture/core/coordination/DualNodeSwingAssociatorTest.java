package com.agoessling.swingcapture.core.coordination;

/** Synthetic-clock coverage for all dual-node association outcomes. */
public final class DualNodeSwingAssociatorTest {
  private static final String SESSION = "swing-session-7";
  private static final long OPENED = 1_000_000;
  private static final long DEADLINE = 2_000_000;
  private static final DualNodeSwingAssociator.Config CONFIG =
      new DualNodeSwingAssociator.Config(1_000, 500, 600, 100_000);

  private DualNodeSwingAssociatorTest() {}

  public static void main(String[] args) {
    pairsExactlyOneReportPerRole();
    rejectsUnrelatedSessionAndExactDuplicate();
    rejectsStaleAndLateReports();
    reportsMissingAndDuplicateRoles();
    rejectsUnrelatedAndAmbiguousTiming();
    rejectsExcessivePairUncertainty();
    rejectsSameNodeAndAmbiguousCandidates();
  }

  private static void pairsExactlyOneReportPerRole() {
    DualNodeSwingAssociator associator = associator();
    submit(associator, report(CaptureRole.DOWN_THE_LINE, "dtl", SESSION, 1_500_000, 100));
    submit(associator, report(CaptureRole.FACE_ON, "face", SESSION, 1_500_500, 100));
    check(
        associator.decide(DEADLINE - 1).status() == AssociationStatus.NOT_READY,
        "association must wait for the explicit deadline");

    AssociationResult result = associator.decide(DEADLINE);
    check(result.status() == AssociationStatus.PAIRED, "one bounded cross-role pair should match");
    AssociatedSwing swing = result.swing().orElseThrow();
    check(swing.minimumTriggerSeparationNs() == 300, "minimum separation");
    check(swing.maximumTriggerSeparationNs() == 700, "maximum separation");
    check(
        swing.downTheLine().report().nodeId().equals("dtl"), "down-the-line role ordering");
    check(swing.faceOn().report().nodeId().equals("face"), "face-on role ordering");
    check(associator.decide(DEADLINE + 10) == result, "terminal decisions must be idempotent");
  }

  private static void rejectsUnrelatedSessionAndExactDuplicate() {
    DualNodeSwingAssociator associator = associator();
    SubmissionResult unrelated =
        associator.submit(
            report(CaptureRole.DOWN_THE_LINE, "dtl", "another-session", 1_500_000, 10),
            ClockOffsetEstimate.identity("dtl"),
            1_500_050);
    check(
        unrelated.status() == SubmissionStatus.UNRELATED_SESSION,
        "another session must not be admitted");

    NodeTriggerReport accepted =
        report(CaptureRole.DOWN_THE_LINE, "dtl", SESSION, 1_500_000, 10);
    submit(associator, accepted);
    SubmissionResult duplicate =
        associator.submit(accepted, ClockOffsetEstimate.identity("dtl"), 1_500_060);
    check(
        duplicate.status() == SubmissionStatus.DUPLICATE_REPORT,
        "an exact replay must be explicit");
  }

  private static void rejectsStaleAndLateReports() {
    DualNodeSwingAssociator associator = associator();
    SubmissionResult stale =
        associator.submit(
            report(CaptureRole.DOWN_THE_LINE, "dtl", SESSION, 1_100_000, 0),
            ClockOffsetEstimate.identity("dtl"),
            1_200_001);
    check(stale.status() == SubmissionStatus.STALE_REPORT, "old arrivals must be stale");

    SubmissionResult late =
        associator.submit(
            report(CaptureRole.FACE_ON, "face", SESSION, 1_900_000, 0),
            ClockOffsetEstimate.identity("face"),
            DEADLINE + 1);
    check(late.status() == SubmissionStatus.LATE_REPORT, "late arrivals must be explicit");
    check(
        associator.decide(DEADLINE + 1).status() == AssociationStatus.MISSING_OR_LATE,
        "rejected arrivals leave an explicit missing/late result");
  }

  private static void reportsMissingAndDuplicateRoles() {
    DualNodeSwingAssociator missing = associator();
    submit(missing, report(CaptureRole.DOWN_THE_LINE, "dtl", SESSION, 1_500_000, 10));
    check(
        missing.decide(DEADLINE).status() == AssociationStatus.MISSING_OR_LATE,
        "one role must not create a swing");

    DualNodeSwingAssociator duplicate = associator();
    submit(duplicate, report(CaptureRole.DOWN_THE_LINE, "dtl-a", SESSION, 1_500_000, 10));
    submit(duplicate, report(CaptureRole.DOWN_THE_LINE, "dtl-b", SESSION, 1_500_500, 10));
    check(
        duplicate.decide(DEADLINE).status() == AssociationStatus.DUPLICATE_ROLE,
        "two reports for one role must not be paired");
  }

  private static void rejectsUnrelatedAndAmbiguousTiming() {
    DualNodeSwingAssociator unrelated = associator();
    submit(unrelated, report(CaptureRole.DOWN_THE_LINE, "dtl", SESSION, 1_500_000, 100));
    submit(unrelated, report(CaptureRole.FACE_ON, "face", SESSION, 1_502_000, 100));
    check(
        unrelated.decide(DEADLINE).status() == AssociationStatus.UNRELATED_TRIGGERS,
        "separated trigger intervals must be unrelated");

    DualNodeSwingAssociator ambiguous = associator();
    submit(ambiguous, report(CaptureRole.DOWN_THE_LINE, "dtl", SESSION, 1_500_000, 100));
    submit(ambiguous, report(CaptureRole.FACE_ON, "face", SESSION, 1_500_900, 100));
    check(
        ambiguous.decide(DEADLINE).status() == AssociationStatus.AMBIGUOUS_TIMING,
        "a tolerance-straddling uncertainty interval must be ambiguous");
  }

  private static void rejectsExcessivePairUncertainty() {
    DualNodeSwingAssociator associator = associator();
    submit(associator, report(CaptureRole.DOWN_THE_LINE, "dtl", SESSION, 1_500_000, 400));
    submit(associator, report(CaptureRole.FACE_ON, "face", SESSION, 1_500_000, 400));
    check(
        associator.decide(DEADLINE).status() == AssociationStatus.EXCESSIVE_UNCERTAINTY,
        "pair uncertainty has an independent gate");

    DualNodeSwingAssociator perReport = associator();
    SubmissionResult result =
        perReport.submit(
            report(CaptureRole.DOWN_THE_LINE, "dtl", SESSION, 1_500_000, 501),
            ClockOffsetEstimate.identity("dtl"),
            1_500_010);
    check(
        result.status() == SubmissionStatus.EXCESSIVE_UNCERTAINTY,
        "per-report uncertainty must be rejected on admission");
  }

  private static void rejectsSameNodeAndAmbiguousCandidates() {
    DualNodeSwingAssociator sameNode = associator();
    submit(sameNode, report(CaptureRole.DOWN_THE_LINE, "node", SESSION, 1_500_000, 10));
    submit(sameNode, report(CaptureRole.FACE_ON, "node", SESSION, 1_500_000, 10));
    check(
        sameNode.decide(DEADLINE).status() == AssociationStatus.SAME_NODE_FOR_BOTH_ROLES,
        "distinct nodes are required");

    DualNodeSwingAssociator ambiguous = associator();
    submit(ambiguous, report(CaptureRole.DOWN_THE_LINE, "dtl-a", SESSION, 1_500_000, 10));
    submit(ambiguous, report(CaptureRole.DOWN_THE_LINE, "dtl-b", SESSION, 1_500_100, 10));
    submit(ambiguous, report(CaptureRole.FACE_ON, "face", SESSION, 1_500_050, 10));
    check(
        ambiguous.decide(DEADLINE).status() == AssociationStatus.AMBIGUOUS_TIMING,
        "multiple plausible cross-role candidates must be ambiguous");
  }

  private static DualNodeSwingAssociator associator() {
    return new DualNodeSwingAssociator(SESSION, OPENED, DEADLINE, CONFIG);
  }

  private static NodeTriggerReport report(
      CaptureRole role, String nodeId, String sessionId, long timestampNs, long uncertaintyNs) {
    return new NodeTriggerReport(role, nodeId, sessionId, timestampNs, uncertaintyNs);
  }

  private static void submit(DualNodeSwingAssociator associator, NodeTriggerReport report) {
    SubmissionResult result =
        associator.submit(
            report,
            ClockOffsetEstimate.identity(report.nodeId()),
            Math.min(report.triggerTimestampNs() + 1_000, DEADLINE));
    check(result.status() == SubmissionStatus.ACCEPTED, "synthetic report should be admitted");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
