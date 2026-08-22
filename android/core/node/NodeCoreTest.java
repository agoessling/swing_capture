package com.agoessling.swingcapture.node;

import java.security.SecureRandom;
import java.util.List;
import java.util.Set;

/** Hermetic tests for node state, credentials, and storage retention. */
public final class NodeCoreTest {
  private NodeCoreTest() {}

  public static void main(String[] arguments) throws Exception {
    credentialsAreStrongAndExact();
    captureStateMachineIsExplicit();
    coordinationStatePreservesSharedSessionIdentity();
    retentionKeepsNewestWithinBothLimits();
    retentionPrioritizesSwingCapturesOverDiagnostics();
    retentionNeverDeletesProtectedSession();
  }

  private static void coordinationStatePreservesSharedSessionIdentity() {
    NodeCoordinationState state = new NodeCoordinationState();
    state.armed("shared-20260815-1");
    state.triggered(
        "down_the_line", "node-1", "local-1", 7_000_000_000L, 350_000L, "local_audio");
    NodeCoordinationState.TriggerReport report = state.latestTrigger();
    assert report != null;
    assert report.sharedSessionId().equals("shared-20260815-1");
    assert report.localSessionId().equals("local-1");
    assert report.timestampUncertaintyNanos() == 350_000L;

    state.resumeMonitoringAfterCapture();
    assert state.sharedSessionId() == null;
    assert state.latestTrigger() == report;
    state.triggered("face_on", "node-2", "ignored", 8_000_000_000L, 0, "manual");
    assert state.latestTrigger() == report;

    state.armed("shared-20260815-2");
    assert state.latestTrigger() == null;

    state.armed(null);
    state.triggered("face_on", "node-2", "local-2", 8_000_000_000L, 0, "manual");
    assert state.latestTrigger() == null;

    boolean missingReportRejected = false;
    try {
      state.resumeMonitoringAfterCapture();
    } catch (IllegalStateException expected) {
      missingReportRejected = true;
    }
    assert missingReportRejected;
  }

  private static void credentialsAreStrongAndExact() throws Exception {
    SecureRandom random = SecureRandom.getInstance("SHA1PRNG");
    random.setSeed(new byte[] {1, 2, 3, 4});
    String token = BearerAuthorization.generate(random);
    assert BearerAuthorization.isValidToken(token);
    assert BearerAuthorization.accepts("Bearer " + token, token);
    assert !BearerAuthorization.accepts("bearer " + token, token);
    assert !BearerAuthorization.accepts("Bearer " + token + "x", token);
  }

  private static void captureStateMachineIsExplicit() {
    CaptureRuntime runtime = new CaptureRuntime();
    assert runtime.snapshot().state() == CaptureRuntime.State.STOPPED;
    runtime.starting();
    runtime.armed();
    runtime.updateRing(5, 0, 50, 1_000_000);
    runtime.transitioningToHighSpeed();
    CaptureRuntime.Snapshot transition = runtime.snapshot();
    assert transition.state() == CaptureRuntime.State.STARTING;
    assert !transition.armed();
    assert transition.videoFrames() == 0;
    runtime.armed();
    runtime.updateRing(480, 96_000, 6_000_000, 2_000_000);
    runtime.triggered("session-1", 4_000_000_000L);
    runtime.publishing();
    runtime.armed();
    CaptureRuntime.Snapshot snapshot = runtime.snapshot();
    assert snapshot.state() == CaptureRuntime.State.ARMED;
    assert snapshot.activeSessionId() == null;
    assert snapshot.videoFrames() == 480;
    assert snapshot.ringDurationUs() == 2_000_000;

    runtime.triggered("session-2", 5_000_000_000L);
    boolean stalePublishing = runtime.publishing("stale-session");
    assert !stalePublishing;
    boolean sessionTwoPublishing = runtime.publishing("session-2");
    assert sessionTwoPublishing;
    runtime.triggered("session-3", 6_000_000_000L);
    boolean stalePublished = runtime.published("session-2");
    assert !stalePublished;
    boolean sessionThreePublishing = runtime.publishing("session-3");
    assert sessionThreePublishing;
    boolean sessionThreePublished = runtime.published("session-3");
    assert sessionThreePublished;
  }

  private static void retentionKeepsNewestWithinBothLimits() {
    List<String> removed =
        SessionRetentionPlanner.deletions(
            List.of(entry("old", 1, 40), entry("middle", 2, 40), entry("new", 3, 40)),
            2,
            70,
            Set.of());
    assert removed.equals(List.of("middle", "old")) : removed;
  }

  private static void retentionNeverDeletesProtectedSession() {
    List<String> removed =
        SessionRetentionPlanner.deletions(
            List.of(entry("old", 1, 100), entry("new", 2, 100)), 1, 50, Set.of("old"));
    assert removed.equals(List.of("new")) : removed;
  }

  private static void retentionPrioritizesSwingCapturesOverDiagnostics() {
    List<String> removed =
        SessionRetentionPlanner.deletions(
            List.of(
                diagnosticEntry("new-diagnostic", 10_000, 10),
                entry("old-capture", 1_000, 10)),
            1,
            100,
            Set.of());
    assert removed.equals(List.of("new-diagnostic")) : removed;
  }

  private static SessionRetentionPlanner.Entry entry(String id, long created, long bytes) {
    return new SessionRetentionPlanner.Entry(id, created, bytes);
  }

  private static SessionRetentionPlanner.Entry diagnosticEntry(
      String id, long created, long bytes) {
    return new SessionRetentionPlanner.Entry(
        id, created, bytes, SessionRetentionPlanner.RetentionClass.DIAGNOSTIC);
  }
}
