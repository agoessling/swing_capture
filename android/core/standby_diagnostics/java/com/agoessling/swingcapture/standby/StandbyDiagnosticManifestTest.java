package com.agoessling.swingcapture.standby;

import com.agoessling.swingcapture.audio.ContinuousAudioImpactDetector;
import com.agoessling.swingcapture.audio.ImpactDetector;
import com.agoessling.swingcapture.diagnostics.DiagnosticIncident;
import com.agoessling.swingcapture.diagnostics.PreviewEvidenceRing;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Optional;
import java.util.OptionalLong;

/** Canonical diagnostic-only manifest and incident contract coverage. */
public final class StandbyDiagnosticManifestTest {
  private StandbyDiagnosticManifestTest() {}

  public static void main(String[] arguments) {
    operatorManifestIsCanonicalAndExplicitlyNonReviewable();
    detectedImpactBuildsImpactWhileNotArmedIncident();
    operatorTagBuildsReviewedMissedShotIncident();
    rejectsInconsistentPathsStatusesBoundsAndMarkers();
  }

  private static void operatorManifestIsCanonicalAndExplicitlyNonReviewable() {
    StandbyDiagnosticCoordinator.EventMarker event = operatorEvent();
    DiagnosticIncident incident =
        StandbyDiagnosticManifest.incidentFor(event, "standby-1", "pixel6-dtl", 1_000);
    int incidentBytes = incident.toCanonicalJson().getBytes(StandardCharsets.UTF_8).length;
    StandbyDiagnosticManifest manifest =
        new StandbyDiagnosticManifest(
            "standby-1",
            1_000,
            "pixel6-dtl",
            StandbyDiagnosticManifest.EventMetadata.from(event),
            audio(90, 105, 99),
            StandbyDiagnosticManifest.PublicationStatus.AVAILABLE,
            Optional.of(preview()),
            StandbyDiagnosticManifest.INCIDENT_FILE_NAME,
            incidentBytes);

    String expected =
        "{\"schema_version\":1,\"session_kind\":\"standby_diagnostic\","
            + "\"session_id\":\"standby-1\",\"created_at_epoch_ms\":\"1000\","
            + "\"source_node_id\":\"pixel6-dtl\",\"event\":{\"sequence\":\"1\","
            + "\"kind\":\"operator_tag\",\"marker_audio_frame_position\":\"99\","
            + "\"audio_clock_status\":\"audio_clock_unvalidated\","
            + "\"marker_boottime_ns\":null,\"marker_uncertainty_ns\":null,"
            + "\"operator_received_boottime_ns\":\"123000000\",\"detector\":null},"
            + "\"evidence\":{\"audio\":{\"path\":\"diagnostic_audio.wav\","
            + "\"content_type\":\"audio/wav\",\"bytes\":\"74\","
            + "\"sample_rate_hz\":48000,\"first_frame_position\":\"90\","
            + "\"end_frame_position\":\"105\",\"marker_frame_position\":\"99\","
            + "\"sample_count\":15,\"marker_sample_index\":9},"
            + "\"preview_status\":\"available\",\"preview\":{"
            + "\"frames_path\":\"pose_diagnostics/preview_frames.mjpeg\","
            + "\"frames_content_type\":\"image/jpeg\",\"frames_bytes\":\"400\","
            + "\"trace_path\":\"pose_diagnostics/pose_trace.ndjson\","
            + "\"trace_content_type\":\"application/x-ndjson\",\"trace_bytes\":200,"
            + "\"first_timestamp_boottime_ns\":\"10\","
            + "\"end_timestamp_boottime_ns_exclusive\":\"21\","
            + "\"observation_count\":2,\"jpeg_frame_count\":2,\"frame_count\":2}},"
            + "\"incident\":{\"path\":\"diagnostic_incident.json\",\"bytes\":"
            + incidentBytes
            + "}}";
    check(manifest.toCanonicalJson().equals(expected), "canonical operator manifest");
    check(
        manifest.toCanonicalJson().contains("\"session_kind\":\"standby_diagnostic\""),
        "explicit diagnostic-only session kind");
  }

  private static void detectedImpactBuildsImpactWhileNotArmedIncident() {
    ImpactDetector.Impact impact =
        new ImpactDetector.Impact(5, 7, 0, 5, 0.8f, 0.01f, 0.05f);
    ContinuousAudioImpactDetector.TimedImpact timed =
        new ContinuousAudioImpactDetector.TimedImpact(
            impact, Optional.empty(), Optional.empty());
    StandbyDiagnosticCoordinator.EventMarker event =
        new StandbyDiagnosticCoordinator.EventMarker(
            2,
            StandbyDiagnosticCoordinator.EventKind.DETECTED_IMPACT,
            5,
            OptionalLong.empty(),
            Optional.of(timed),
            Optional.empty(),
            emptyPreview());

    DiagnosticIncident incident =
        StandbyDiagnosticManifest.incidentFor(event, "impact-2", "pixel5a-atl", 2_000);

    check(
        incident.classification()
            == DiagnosticIncident.IncidentClassification.IMPACT_WHILE_NOT_ARMED,
        "detected incident classification");
    check(
        incident.userFeedback().classification()
            == DiagnosticIncident.FeedbackClassification.UNREVIEWED,
        "detected impact initially unreviewed");
    check(incident.timingMarks().size() == 1, "detected impact timing mark");
    check(
        incident.timingMarks().get(0).kind()
            == DiagnosticIncident.TimingMarkKind.AUDIO_IMPACT_TRANSIENT,
        "detected audio timing kind");
    check(incident.timingMarks().get(0).offsetMicros() == 0, "detected audio mark origin");
    StandbyDiagnosticManifest.EventMetadata metadata =
        StandbyDiagnosticManifest.EventMetadata.from(event);
    check(metadata.detector().isPresent(), "detector evidence present");
    check(
        metadata.audioClockStatus()
            == StandbyDiagnosticCoordinator.AudioClockStatus.UNVALIDATED,
        "untimed detector status explicit");
  }

  private static void operatorTagBuildsReviewedMissedShotIncident() {
    DiagnosticIncident incident =
        StandbyDiagnosticManifest.incidentFor(
            operatorEvent(), "operator-1", "pixel6-dtl", 3_000);

    check(
        incident.classification() == DiagnosticIncident.IncidentClassification.USER_REPORTED,
        "operator incident classification");
    check(
        incident.userFeedback().classification()
            == DiagnosticIncident.FeedbackClassification.MISSED_SHOT,
        "operator tag records missed-shot feedback");
    check(incident.timingMarks().isEmpty(), "operator does not invent impact timing");
  }

  private static void rejectsInconsistentPathsStatusesBoundsAndMarkers() {
    StandbyDiagnosticCoordinator.EventMarker event = operatorEvent();
    StandbyDiagnosticManifest.EventMetadata metadata =
        StandbyDiagnosticManifest.EventMetadata.from(event);
    expectThrows(
        IllegalArgumentException.class,
        () ->
            new StandbyDiagnosticManifest(
                "standby",
                1,
                "node",
                metadata,
                audio(90, 105, 99),
                StandbyDiagnosticManifest.PublicationStatus.AVAILABLE,
                Optional.empty(),
                StandbyDiagnosticManifest.INCIDENT_FILE_NAME,
                1),
        "available preview without metadata");
    expectThrows(
        IllegalArgumentException.class,
        () ->
            new StandbyDiagnosticManifest(
                "standby",
                1,
                "node",
                metadata,
                audio(90, 105, 98),
                StandbyDiagnosticManifest.PublicationStatus.NOT_AVAILABLE,
                Optional.empty(),
                StandbyDiagnosticManifest.INCIDENT_FILE_NAME,
                1),
        "audio marker mismatch");
    expectThrows(
        IllegalArgumentException.class,
        () ->
            new StandbyDiagnosticManifest.PreviewArtifact(
                "../frames.mjpeg",
                StandbyDiagnosticManifest.PREVIEW_FRAME_CONTENT_TYPE,
                1,
                StandbyDiagnosticManifest.PREVIEW_TRACE_PATH,
                StandbyDiagnosticManifest.PREVIEW_TRACE_CONTENT_TYPE,
                1,
                1,
                2,
                1,
                1),
        "unsafe preview path");
    expectThrows(
        IllegalArgumentException.class,
        () -> audio(90, 105, 105),
        "audio marker outside window");
  }

  private static StandbyDiagnosticCoordinator.EventMarker operatorEvent() {
    return new StandbyDiagnosticCoordinator.EventMarker(
        1,
        StandbyDiagnosticCoordinator.EventKind.OPERATOR_TAG,
        99,
        OptionalLong.of(123_000_000L),
        Optional.empty(),
        Optional.empty(),
        emptyPreview());
  }

  private static StandbyDiagnosticManifest.AudioArtifact audio(
      long first, long end, long marker) {
    int samples = Math.toIntExact(end - first);
    return new StandbyDiagnosticManifest.AudioArtifact(
        StandbyDiagnosticManifest.AUDIO_FILE_NAME,
        StandbyDiagnosticManifest.AUDIO_CONTENT_TYPE,
        StandbyDiagnosticManifest.AudioArtifact.expectedWavBytes(samples),
        StandbyDiagnosticCoordinator.SAMPLE_RATE_HZ,
        first,
        end,
        marker,
        samples,
        Math.toIntExact(marker - first));
  }

  private static StandbyDiagnosticManifest.PreviewArtifact preview() {
    return new StandbyDiagnosticManifest.PreviewArtifact(
        StandbyDiagnosticManifest.PREVIEW_FRAMES_PATH,
        StandbyDiagnosticManifest.PREVIEW_FRAME_CONTENT_TYPE,
        400,
        StandbyDiagnosticManifest.PREVIEW_TRACE_PATH,
        StandbyDiagnosticManifest.PREVIEW_TRACE_CONTENT_TYPE,
        200,
        10,
        21,
        2,
        2);
  }

  private static PreviewEvidenceRing.Snapshot emptyPreview() {
    return new PreviewEvidenceRing(1, 1, 1).snapshot();
  }

  private static <T extends Throwable> void expectThrows(
      Class<T> expected, Runnable action, String message) {
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
