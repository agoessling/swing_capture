package com.agoessling.swingcapture;

import com.agoessling.swingcapture.diagnostics.DiagnosticAudioRing;
import com.agoessling.swingcapture.diagnostics.DiagnosticIncident;
import com.agoessling.swingcapture.diagnostics.PreviewEvidence;
import com.agoessling.swingcapture.diagnostics.PreviewEvidenceRing;
import com.agoessling.swingcapture.standby.StandbyDiagnosticCoordinator;
import com.agoessling.swingcapture.standby.StandbyDiagnosticManifest;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.OptionalLong;

/** Host coverage for atomic diagnostic-only session publication and preview isolation. */
public final class StandbyDiagnosticSessionPublisherTest {
  private StandbyDiagnosticSessionPublisherTest() {}

  public static void main(String[] arguments) throws Exception {
    publishesDurableAudioPreviewManifestAndIncident();
    malformedPreviewIsRecordedWithoutFailingPrimarySession();
    previewSyncFailureIsCleanedWithoutFailingPrimarySession();
    emptyPreviewIsExplicitlyNotAvailable();
    primarySyncFailureCleansUnpublishedSession();
    rejectsUnsafeAndConflictingSessionPaths();
  }

  private static void publishesDurableAudioPreviewManifestAndIncident() throws Exception {
    File sessions = sessionsDirectory("available");
    List<String> synchronizedDirectories = new ArrayList<>();
    StandbyDiagnosticSessionPublisher publisher =
        new StandbyDiagnosticSessionPublisher(
            directory -> synchronizedDirectories.add(directory.getName()));

    StandbyDiagnosticSessionPublisher.Result result =
        publisher.publish(
            sessions,
            "operator-1",
            "pixel6-dtl",
            1_786_000_000_000L,
            frozenOperator(validPreview()));

    check(result.sessionDirectory().equals(new File(sessions, "operator-1")), "result directory");
    check(result.sessionDirectory().isDirectory(), "published session exists");
    check(!new File(sessions, "operator-1.tmp").exists(), "temporary session removed");
    check(
        synchronizedDirectories.equals(
            List.of(
                "operator-1.tmp",
                "pose_diagnostics.tmp",
                "operator-1.tmp",
                "operator-1.tmp",
                "sessions")),
        "durability synchronization order");
    check(
        result.manifest().previewStatus()
            == StandbyDiagnosticManifest.PublicationStatus.AVAILABLE,
        "preview available status");
    check(result.manifest().preview().isPresent(), "preview metadata present");
    check(result.previewFailureType().isEmpty(), "no preview failure");
    check(
        result.incident().classification()
            == DiagnosticIncident.IncidentClassification.USER_REPORTED,
        "operator incident classification");
    check(
        result.incident().userFeedback().classification()
            == DiagnosticIncident.FeedbackClassification.MISSED_SHOT,
        "operator missed-shot feedback");

    File session = result.sessionDirectory();
    String manifest =
        Files.readString(new File(session, "manifest.json").toPath(), StandardCharsets.UTF_8);
    check(manifest.equals(result.manifest().toCanonicalJson() + "\n"), "exact manifest bytes");
    String incident =
        Files.readString(
            new File(session, StandbyDiagnosticManifest.INCIDENT_FILE_NAME).toPath(),
            StandardCharsets.UTF_8);
    check(incident.equals(result.incident().toCanonicalJson()), "exact incident bytes");
    check(DiagnosticIncident.fromCanonicalJson(incident).equals(result.incident()),
        "incident strict round trip");
    File wav = new File(session, StandbyDiagnosticManifest.AUDIO_FILE_NAME);
    check(wav.length() == 64, "WAV byte length");
    byte[] wavBytes = Files.readAllBytes(wav.toPath());
    check(new String(wavBytes, 0, 4, StandardCharsets.US_ASCII).equals("RIFF"), "WAV RIFF");
    check(wavBytes[44 + 5 * 2] == 0x34 && wavBytes[44 + 5 * 2 + 1] == 0x12,
        "WAV marker sample bytes");
    check(new File(session, PoseDiagnosticFiles.DIRECTORY_NAME).isDirectory(),
        "pose evidence published");
  }

  private static void malformedPreviewIsRecordedWithoutFailingPrimarySession() throws Exception {
    File sessions = sessionsDirectory("preview-failed");
    StandbyDiagnosticSessionPublisher.Result result =
        new StandbyDiagnosticSessionPublisher(ignored -> {})
            .publish(
                sessions,
                "operator-2",
                "pixel6-dtl",
                2_000,
                frozenOperator(malformedPreview()));

    check(result.sessionDirectory().isDirectory(), "primary session survives preview failure");
    check(
        result.manifest().previewStatus()
            == StandbyDiagnosticManifest.PublicationStatus.PUBLICATION_FAILED,
        "preview publication-failed status");
    check(result.manifest().preview().isEmpty(), "failed preview metadata absent");
    check(
        result.previewFailureType().equals(Optional.of("java.lang.IllegalArgumentException")),
        "bounded preview failure type");
    check(!new File(result.sessionDirectory(), PoseDiagnosticFiles.DIRECTORY_NAME).exists(),
        "malformed preview artifacts absent");
    check(new File(result.sessionDirectory(), "manifest.json").isFile(), "primary manifest");
    check(new File(result.sessionDirectory(), StandbyDiagnosticManifest.AUDIO_FILE_NAME).isFile(),
        "primary diagnostic WAV");
  }

  private static void emptyPreviewIsExplicitlyNotAvailable() throws Exception {
    File sessions = sessionsDirectory("preview-empty");
    StandbyDiagnosticSessionPublisher.Result result =
        new StandbyDiagnosticSessionPublisher(ignored -> {})
            .publish(
                sessions,
                "operator-3",
                "pixel6-dtl",
                3_000,
                frozenOperator(emptyPreview()));

    check(
        result.manifest().previewStatus()
            == StandbyDiagnosticManifest.PublicationStatus.NOT_AVAILABLE,
        "empty preview status");
    check(result.manifest().preview().isEmpty(), "empty preview metadata absent");
    check(result.previewFailureType().isEmpty(), "empty preview is not a failure");
  }

  private static void previewSyncFailureIsCleanedWithoutFailingPrimarySession()
      throws Exception {
    File sessions = sessionsDirectory("preview-sync-failed");
    int[] calls = new int[1];
    StandbyDiagnosticSessionPublisher.Result result =
        new StandbyDiagnosticSessionPublisher(
                ignored -> {
                  ++calls[0];
                  if (calls[0] == 3) {
                    throw new IOException("synthetic post-rename preview sync failure");
                  }
                })
            .publish(
                sessions,
                "operator-sync",
                "pixel6-dtl",
                2_500,
                frozenOperator(validPreview()));

    check(result.sessionDirectory().isDirectory(), "preview sync failure preserves primary");
    check(
        result.manifest().previewStatus()
            == StandbyDiagnosticManifest.PublicationStatus.PUBLICATION_FAILED,
        "preview sync failure status");
    check(
        result.previewFailureType().equals(Optional.of("java.io.IOException")),
        "preview sync failure type");
    check(
        !new File(result.sessionDirectory(), PoseDiagnosticFiles.DIRECTORY_NAME).exists(),
        "post-rename preview failure artifacts cleaned");
    check(
        !new File(result.sessionDirectory(), PoseDiagnosticFiles.DIRECTORY_NAME + ".tmp").exists(),
        "preview staging absent after failure");
  }

  private static void primarySyncFailureCleansUnpublishedSession() throws Exception {
    File sessions = sessionsDirectory("sync-failed");
    StandbyDiagnosticSessionPublisher publisher =
        new StandbyDiagnosticSessionPublisher(
            ignored -> {
              throw new IOException("synthetic directory sync failure");
            });

    expectThrows(
        IOException.class,
        () ->
            publisher.publish(
                sessions,
                "operator-4",
                "pixel6-dtl",
                4_000,
                frozenOperator(emptyPreview())),
        "primary sync failure");
    check(!new File(sessions, "operator-4").exists(), "failed primary not published");
    check(!new File(sessions, "operator-4.tmp").exists(), "failed primary staging cleaned");
  }

  private static void rejectsUnsafeAndConflictingSessionPaths() throws Exception {
    File sessions = sessionsDirectory("paths");
    StandbyDiagnosticSessionPublisher publisher =
        new StandbyDiagnosticSessionPublisher(ignored -> {});
    expectThrows(
        IllegalArgumentException.class,
        () ->
            publisher.publish(
                sessions,
                "../escape",
                "node",
                1,
                frozenOperator(emptyPreview())),
        "unsafe session id");
    expectThrows(
        IllegalArgumentException.class,
        () ->
            publisher.publish(
                sessions,
                "safe-session",
                "../unsafe-node",
                1,
                frozenOperator(emptyPreview())),
        "unsafe source node");
    check(!new File(sessions, "safe-session.tmp").exists(), "invalid source creates no staging");
    expectThrows(
        IllegalArgumentException.class,
        () ->
            publisher.publish(
                sessions,
                "bad-time",
                "node",
                0,
                frozenOperator(emptyPreview())),
        "nonpositive creation time");
    check(!new File(sessions, "bad-time.tmp").exists(), "invalid time creates no staging");
    File existing = new File(sessions, "existing");
    check(existing.mkdir(), "create existing session");
    expectThrows(
        IOException.class,
        () ->
            publisher.publish(
                sessions,
                "existing",
                "node",
                1,
                frozenOperator(emptyPreview())),
        "existing session");
    check(existing.isDirectory(), "existing session preserved");
  }

  private static StandbyDiagnosticCoordinator.FrozenEvidence frozenOperator(
      PreviewEvidenceRing.Snapshot preview) {
    DiagnosticAudioRing ring = new DiagnosticAudioRing(100);
    short[] samples = new short[10];
    samples[5] = 0x1234;
    ring.append(samples, 0, samples.length, 100);
    StandbyDiagnosticCoordinator.EventMarker event =
        new StandbyDiagnosticCoordinator.EventMarker(
            1,
            StandbyDiagnosticCoordinator.EventKind.OPERATOR_TAG,
            105,
            OptionalLong.of(9_000_000_000L),
            Optional.empty(),
            Optional.empty(),
            preview);
    return new StandbyDiagnosticCoordinator.FrozenEvidence(event, ring.snapshot(100, 110));
  }

  private static PreviewEvidenceRing.Snapshot validPreview() {
    return preview(new byte[] {(byte) 0xff, (byte) 0xd8, 1, (byte) 0xff, (byte) 0xd9});
  }

  private static PreviewEvidenceRing.Snapshot malformedPreview() {
    return preview(new byte[] {1, 2, 3, 4});
  }

  private static PreviewEvidenceRing.Snapshot preview(byte[] frame) {
    PreviewEvidenceRing ring = new PreviewEvidenceRing(100, 1, 100);
    ring.append(
        new PreviewEvidence(
            10,
            frame,
            "model",
            1,
            0.5,
            0.5,
            0.0,
            true,
            PreviewEvidence.ControllerState.MONITORING,
            "waiting"));
    return ring.snapshot();
  }

  private static PreviewEvidenceRing.Snapshot emptyPreview() {
    return new PreviewEvidenceRing(1, 1, 1).snapshot();
  }

  private static File sessionsDirectory(String name) throws IOException {
    File root = new File(System.getenv("TEST_TMPDIR"), name);
    Files.createDirectory(root.toPath());
    File sessions = new File(root, "sessions");
    Files.createDirectory(sessions.toPath());
    return sessions;
  }

  @FunctionalInterface
  private interface ThrowingRunnable {
    void run() throws Exception;
  }

  private static <T extends Throwable> void expectThrows(
      Class<T> expected, ThrowingRunnable action, String message) {
    try {
      action.run();
    } catch (Throwable thrown) {
      if (expected.isInstance(thrown)) {
        return;
      }
      throw new AssertionError(message + " threw " + thrown, thrown);
    }
    throw new AssertionError(message + " did not throw " + expected.getSimpleName());
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
