package com.agoessling.swingcapture;

import com.agoessling.swingcapture.diagnostics.DiagnosticIncident;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicInteger;

/** Atomic incident initialization, strict feedback, and winner-preserving failure coverage. */
public final class SessionDiagnosticStoreTest {
  private SessionDiagnosticStoreTest() {}

  public static void main(String[] arguments) throws Exception {
    initializesAndUpdatesCanonicalIncident();
    malformedFeedbackPreservesExistingIncident();
    malformedUtf8IsRejectedWithoutReplacingIncident();
    localTimingBoundsAreInclusiveAndWinnerPreserving();
    malformedStoredUtf8IsRejected();
    missedShotHasExplicitInitialClassification();
    peerAudioTriggersAreSuccessfulCaptures();
  }

  private static void initializesAndUpdatesCanonicalIncident() throws Exception {
    File session = sessionDirectory("updated", "session-a");
    AtomicInteger syncs = new AtomicInteger();
    SessionDiagnosticStore store = new SessionDiagnosticStore(ignored -> syncs.incrementAndGet());
    DiagnosticIncident initial =
        store.initialize(session, "session-a", "node-a", "local_audio", 1234);
    check(
        initial.classification() == DiagnosticIncident.IncidentClassification.SUCCESSFUL_CAPTURE,
        "successful initial classification");
    check(syncs.get() == 1, "initial directory sync");

    byte[] request =
        ("{\"schema_version\":1,\"classification\":\"av_sync_wrong\","
                + "\"note\":\"audio is early\",\"timing_marks_us\":{"
                + "\"visual_impact_us\":1250,\"audio_impact_us\":-750}}")
            .getBytes(StandardCharsets.UTF_8);
    DiagnosticIncident updated = store.applyFeedback(session, "session-a", request);
    check(updated.userFeedback().classification().wireName().equals("av_sync_wrong"), "feedback");
    check(updated.userFeedback().note().equals("audio is early"), "feedback note");
    check(updated.timingMarks().size() == 2, "timing marks");
    check(syncs.get() == 2, "update directory sync");
    check(store.read(session, "session-a").equals(updated), "durable updated incident");
  }

  private static void malformedFeedbackPreservesExistingIncident() throws Exception {
    File session = sessionDirectory("preserve", "session-b");
    SessionDiagnosticStore store = new SessionDiagnosticStore(ignored -> {});
    store.initialize(session, "session-b", "node-b", "manual", 2345);
    byte[] before = Files.readAllBytes(new File(session, SessionDiagnosticStore.FILE_NAME).toPath());
    try {
      store.applyFeedback(
          session,
          "session-b",
          "{\"schema_version\":1,\"classification\":\"unknown\"}"
              .getBytes(StandardCharsets.UTF_8));
      throw new AssertionError("Expected malformed feedback rejection");
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
    byte[] after = Files.readAllBytes(new File(session, SessionDiagnosticStore.FILE_NAME).toPath());
    check(java.util.Arrays.equals(before, after), "malformed request preserves winner");
  }

  private static void malformedUtf8IsRejectedWithoutReplacingIncident() throws Exception {
    File session = sessionDirectory("invalid-utf8", "session-utf8");
    SessionDiagnosticStore store = new SessionDiagnosticStore(ignored -> {});
    store.initialize(session, "session-utf8", "node-utf8", "manual", 2_500);
    File incident = new File(session, SessionDiagnosticStore.FILE_NAME);
    byte[] before = Files.readAllBytes(incident.toPath());
    byte[] prefix =
        "{\"schema_version\":1,\"classification\":\"other\",\"note\":\""
            .getBytes(StandardCharsets.UTF_8);
    byte[] suffix = "\"}".getBytes(StandardCharsets.UTF_8);
    byte[] malformed = new byte[prefix.length + 2 + suffix.length];
    System.arraycopy(prefix, 0, malformed, 0, prefix.length);
    malformed[prefix.length] = (byte) 0xc3;
    malformed[prefix.length + 1] = 0x28;
    System.arraycopy(suffix, 0, malformed, prefix.length + 2, suffix.length);

    expectInvalid(
        () -> store.applyFeedback(session, "session-utf8", malformed),
        "malformed request UTF-8");
    check(java.util.Arrays.equals(before, Files.readAllBytes(incident.toPath())),
        "invalid UTF-8 preserves incident");
  }

  private static void localTimingBoundsAreInclusiveAndWinnerPreserving() throws Exception {
    File session = sessionDirectory("timing-bounds", "session-timing");
    SessionDiagnosticStore store = new SessionDiagnosticStore(ignored -> {});
    store.initialize(session, "session-timing", "node-timing", "manual", 2_600);
    SessionDiagnosticStore.LocalTimingBounds bounds =
        new SessionDiagnosticStore.LocalTimingBounds(-2_000_000, 0);
    SessionDiagnosticStore.LocalTimingPolicy policy =
        new SessionDiagnosticStore.LocalTimingPolicy(
            Optional.of(bounds),
            Optional.of(new SessionDiagnosticStore.LocalTimingBounds(-1_400_000, 500_000)),
            Optional.of(new SessionDiagnosticStore.LocalTimingBounds(-10_000_000, 3_000_000)));
    byte[] boundaryRequest =
        ("{\"schema_version\":1,\"classification\":\"good_capture\","
                + "\"timing_marks_us\":{\"desired_high_speed_start_us\":-2000000,"
                + "\"audio_impact_us\":3000000}}")
            .getBytes(StandardCharsets.UTF_8);
    DiagnosticIncident bounded =
        store.applyFeedback(session, "session-timing", boundaryRequest, policy);
    check(bounded.timingMarks().size() == 2, "inclusive timing bounds");
    byte[] winner =
        Files.readAllBytes(new File(session, SessionDiagnosticStore.FILE_NAME).toPath());

    byte[] outside =
        ("{\"schema_version\":1,\"classification\":\"armed_too_late\","
                + "\"timing_marks_us\":{\"audio_impact_us\":3000001}}")
            .getBytes(StandardCharsets.UTF_8);
    expectInvalid(
        () -> store.applyFeedback(session, "session-timing", outside, policy),
        "audio timing outside independent WAV bounds");
    check(
        java.util.Arrays.equals(
            winner,
            Files.readAllBytes(new File(session, SessionDiagnosticStore.FILE_NAME).toPath())),
        "out-of-range timing preserves winner");

    byte[] unboundedDefault =
        ("{\"schema_version\":1,\"classification\":\"other\","
                + "\"timing_marks_us\":{\"audio_impact_us\":60000001}}")
            .getBytes(StandardCharsets.UTF_8);
    DiagnosticIncident unbounded =
        store.applyFeedback(session, "session-timing", unboundedDefault);
    check(unbounded.timingMarks().get(0).offsetMicros() == 60_000_001,
        "legacy overload leaves artifact-specific validation to caller");
    expectInvalid(
        () -> new SessionDiagnosticStore.LocalTimingBounds(1, 2),
        "timing bounds exclude impact");
  }

  private static void malformedStoredUtf8IsRejected() throws Exception {
    File session = sessionDirectory("stored-utf8", "session-stored");
    File incident = new File(session, SessionDiagnosticStore.FILE_NAME);
    Files.write(incident.toPath(), new byte[] {(byte) 0xc3, 0x28});
    SessionDiagnosticStore store = new SessionDiagnosticStore(ignored -> {});
    expectIo(() -> store.read(session, "session-stored"), "malformed stored UTF-8");
  }

  private static void missedShotHasExplicitInitialClassification() throws Exception {
    File session = sessionDirectory("missed", "session-c");
    SessionDiagnosticStore store = new SessionDiagnosticStore(ignored -> {});
    DiagnosticIncident incident =
        store.initialize(session, "session-c", "node-c", "missed_shot", 3456);
    check(
        incident.classification()
            == DiagnosticIncident.IncidentClassification.USER_REPORTED,
        "missed shot classification");

    File noImpactSession = sessionDirectory("pose-no-impact", "session-pose");
    DiagnosticIncident noImpact =
        store.initialize(
            noImpactSession, "session-pose", "node-1", "pose_armed_no_impact", 1_234L);
    check(
        noImpact.classification()
            == DiagnosticIncident.IncidentClassification.POSE_ARMED_NO_IMPACT,
        "pose no-impact classification");
  }

  private static void peerAudioTriggersAreSuccessfulCaptures() throws Exception {
    String[] sources = {
      "peer_audio_arrival", "peer_audio_local_candidate", "peer_audio_clock_candidate"
    };
    for (int index = 0; index < sources.length; ++index) {
      String sessionId = "peer-" + index;
      File session = sessionDirectory("peer-audio-" + index, sessionId);
      DiagnosticIncident incident =
          new SessionDiagnosticStore(ignored -> {})
              .initialize(session, sessionId, "shadow-node", sources[index], 4_000 + index);
      check(
          incident.classification()
              == DiagnosticIncident.IncidentClassification.SUCCESSFUL_CAPTURE,
          "peer audio capture classification");
    }
  }

  private static File sessionDirectory(String test, String sessionId) throws Exception {
    File root = new File(System.getenv("TEST_TMPDIR"), test);
    check(root.mkdirs(), "create test root");
    File session = new File(root, sessionId);
    check(session.mkdir(), "create session");
    return session;
  }

  private static void expectInvalid(ThrowingAction action, String label) throws Exception {
    try {
      action.run();
      throw new AssertionError("Expected IllegalArgumentException: " + label);
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
  }

  private static void expectIo(ThrowingAction action, String label) throws Exception {
    try {
      action.run();
      throw new AssertionError("Expected IOException: " + label);
    } catch (java.io.IOException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String label) {
    if (!condition) {
      throw new AssertionError(label);
    }
  }

  @FunctionalInterface
  private interface ThrowingAction {
    void run() throws Exception;
  }
}
