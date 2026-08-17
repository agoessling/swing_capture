package com.agoessling.swingcapture.diagnostics;

import com.agoessling.swingcapture.diagnostics.DiagnosticIncident.FeedbackClassification;
import com.agoessling.swingcapture.diagnostics.DiagnosticIncident.IncidentClassification;
import com.agoessling.swingcapture.diagnostics.DiagnosticIncident.TimingMark;
import com.agoessling.swingcapture.diagnostics.DiagnosticIncident.TimingMarkKind;
import com.agoessling.swingcapture.diagnostics.DiagnosticIncident.UserFeedback;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;

/** Canonical serialization plus strict malformed-contract and browser-request coverage. */
public final class DiagnosticIncidentTest {
  private static final String CANONICAL =
      "{\"schema_version\":1,\"incident_id\":\"incident-17\","
          + "\"classification\":\"successful_capture\","
          + "\"created_at_epoch_ms\":\"1786900000123\","
          + "\"source_node_id\":\"pixel6-dtl\",\"user_feedback\":{"
          + "\"classification\":\"av_sync_wrong\",\"note\":\"Audio \\\"late\\\"\\nby eye\"},"
          + "\"timing_marks\":[{\"kind\":\"high_speed_should_start\","
          + "\"stream_id\":\"preview\",\"offset_us\":\"-700000\"},{"
          + "\"kind\":\"visual_ball_impact\",\"stream_id\":\"high_speed\","
          + "\"offset_us\":\"0\"},{\"kind\":\"audio_impact_transient\","
          + "\"stream_id\":\"audio\",\"offset_us\":\"120\"}]}";

  private DiagnosticIncidentTest() {}

  public static void main(String[] arguments) {
    canonicalSerializationSortsAndRoundTrips();
    unicodeFeedbackRoundTrips();
    recordCollectionsAreDetachedAndImmutable();
    malformedAndNoncanonicalIncidentsAreRejected();
    browserFeedbackRequestParsesExactSignedContract();
    malformedBrowserFeedbackRequestsAreRejected();
    constructorBoundsAndDuplicatesAreRejected();
  }

  private static void canonicalSerializationSortsAndRoundTrips() {
    DiagnosticIncident incident = incident();
    check(incident.toCanonicalJson().equals(CANONICAL), "canonical JSON");
    DiagnosticIncident parsed = DiagnosticIncident.fromCanonicalJson(CANONICAL);
    check(parsed.equals(incident), "canonical round trip");
    check(parsed.timingMarks().get(0).offsetMicros() == -700_000, "signed mark retained");
  }

  private static void unicodeFeedbackRoundTrips() {
    DiagnosticIncident unicode =
        new DiagnosticIncident(
            "unicode",
            IncidentClassification.USER_REPORTED,
            1,
            "node",
            new UserFeedback(FeedbackClassification.OTHER, "Impact 🏌️"),
            List.of());
    check(
        DiagnosticIncident.fromCanonicalJson(unicode.toCanonicalJson()).equals(unicode),
        "Unicode feedback round trip");
  }

  private static void recordCollectionsAreDetachedAndImmutable() {
    ArrayList<TimingMark> source = new ArrayList<>();
    source.add(mark(TimingMarkKind.VISUAL_BALL_IMPACT, "high_speed", 0));
    DiagnosticIncident incident =
        new DiagnosticIncident(
            "id",
            IncidentClassification.USER_REPORTED,
            1,
            "node",
            UserFeedback.unreviewed(),
            source);
    source.clear();
    check(incident.timingMarks().size() == 1, "constructor detached marks");
    expectUnsupported(() -> incident.timingMarks().clear(), "timing mark list mutation");
  }

  private static void malformedAndNoncanonicalIncidentsAreRejected() {
    expectIllegalArgument(
        () -> DiagnosticIncident.fromCanonicalJson(" " + CANONICAL), "noncanonical whitespace");
    expectIllegalArgument(
        () ->
            DiagnosticIncident.fromCanonicalJson(
                CANONICAL.replace("\"schema_version\":1", "\"schema_version\":2")),
        "unsupported schema");
    expectIllegalArgument(
        () ->
            DiagnosticIncident.fromCanonicalJson(
                CANONICAL.replace(
                    "\"schema_version\":1",
                    "\"schema_version\":1,\"schema_version\":1")),
        "duplicate field");
    expectIllegalArgument(
        () ->
            DiagnosticIncident.fromCanonicalJson(
                CANONICAL.replace(
                    "\"classification\":\"successful_capture\"",
                    "\"classification\":\"not_real\"")),
        "unknown classification");
    expectIllegalArgument(
        () ->
            DiagnosticIncident.fromCanonicalJson(
                CANONICAL.replace(
                    "\"created_at_epoch_ms\":\"1786900000123\"",
                    "\"created_at_epoch_ms\":1786900000123")),
        "64-bit incident field as number");
    expectIllegalArgument(
        () ->
            DiagnosticIncident.fromCanonicalJson(
                CANONICAL.replace("\"offset_us\":\"120\"", "\"offset_us\":\"0120\"")),
        "noncanonical decimal");
    expectIllegalArgument(
        () -> DiagnosticIncident.fromCanonicalJson(CANONICAL + "x"), "trailing content");
    String oversized = "x".repeat(DiagnosticIncident.MAXIMUM_SERIALIZED_BYTES + 1);
    expectIllegalArgument(
        () -> DiagnosticIncident.fromCanonicalJson(oversized), "oversized incident JSON");
  }

  private static void browserFeedbackRequestParsesExactSignedContract() {
    String json =
        "{\"schema_version\":1,\"classification\":\"av_sync_wrong\","
            + "\"note\":\"audio trails video\",\"timing_marks_us\":{"
            + "\"desired_high_speed_start_us\":-750000,\"visual_impact_us\":0,"
            + "\"audio_impact_us\":230}}";
    DiagnosticFeedbackRequest request = DiagnosticFeedbackRequest.parseJson(json);
    check(request.classification() == FeedbackClassification.AUDIO_VIDEO_SYNC_WRONG,
        "web classification");
    check(request.note().equals(Optional.of("audio trails video")), "optional note");
    check(request.timingMarks().size() == 3, "three timing labels");
    check(request.timingMarks().get(0).offsetMicros() == -750_000, "signed web label");

    DiagnosticIncident updated = request.applyTo(unreviewedIncident());
    check(updated.userFeedback().classification() == FeedbackClassification.AUDIO_VIDEO_SYNC_WRONG,
        "request applies classification");
    check(updated.userFeedback().note().equals("audio trails video"), "request applies note");
    check(updated.timingMarks().size() == 3, "request applies marks");

    DiagnosticFeedbackRequest minimal =
        DiagnosticFeedbackRequest.parseJson(
            "{\"classification\":\"good_capture\",\"schema_version\":1}");
    check(minimal.note().isEmpty(), "note may be omitted");
    check(minimal.timingMarks().isEmpty(), "marks may be omitted");
  }

  private static void malformedBrowserFeedbackRequestsAreRejected() {
    expectBadRequest(
        "{\"schema_version\":1,\"classification\":\"unreviewed\"}",
        "unreviewed request");
    expectBadRequest("{\"schema_version\":1}", "missing classification");
    expectBadRequest(
        "{\"schema_version\":1,\"classification\":\"good_capture\",\"extra\":1}",
        "unknown root field");
    expectBadRequest(
        "{\"schema_version\":1,\"classification\":\"good_capture\","
            + "\"timing_marks_us\":{\"visual_impact_us\":\"1\"}}",
        "string timing value");
    expectBadRequest(
        "{\"schema_version\":1,\"classification\":\"good_capture\","
            + "\"timing_marks_us\":{\"unknown_us\":1}}",
        "unknown timing field");
    String longNote = "x".repeat(DiagnosticIncident.MAXIMUM_NOTE_CODE_UNITS + 1);
    expectBadRequest(
        "{\"schema_version\":1,\"classification\":\"other\",\"note\":\""
            + longNote
            + "\"}",
        "note over UI limit");
    expectBadRequest(
        "{\"schema_version\":1,\"classification\":\"good_capture\","
            + "\"classification\":\"other\"}",
        "duplicate request field");
  }

  private static void constructorBoundsAndDuplicatesAreRejected() {
    expectIllegalArgument(
        () ->
            new UserFeedback(
                FeedbackClassification.GOOD_CAPTURE,
                "x".repeat(DiagnosticIncident.MAXIMUM_NOTE_CODE_UNITS + 1)),
        "feedback note over UI limit");
    expectIllegalArgument(
        () -> new UserFeedback(FeedbackClassification.UNREVIEWED, "note"),
        "unreviewed note");
    expectIllegalArgument(
        () ->
            new DiagnosticIncident(
                "bad/id",
                IncidentClassification.USER_REPORTED,
                1,
                "node",
                UserFeedback.unreviewed(),
                List.of()),
        "invalid incident identifier");
    expectIllegalArgument(
        () ->
            new DiagnosticIncident(
                "id",
                IncidentClassification.USER_REPORTED,
                1,
                "node",
                UserFeedback.unreviewed(),
                List.of(
                    mark(TimingMarkKind.VISUAL_BALL_IMPACT, "high_speed", 1),
                    mark(TimingMarkKind.VISUAL_BALL_IMPACT, "high_speed", 2))),
        "duplicate timing kind and stream");
    ArrayList<TimingMark> tooMany = new ArrayList<>();
    for (int index = 0; index <= DiagnosticIncident.MAXIMUM_TIMING_MARKS; ++index) {
      tooMany.add(mark(TimingMarkKind.VISUAL_BALL_IMPACT, "stream" + index, index));
    }
    expectIllegalArgument(
        () ->
            new DiagnosticIncident(
                "id",
                IncidentClassification.USER_REPORTED,
                1,
                "node",
                UserFeedback.unreviewed(),
                tooMany),
        "too many timing marks");
  }

  private static DiagnosticIncident incident() {
    return new DiagnosticIncident(
        "incident-17",
        IncidentClassification.SUCCESSFUL_CAPTURE,
        1_786_900_000_123L,
        "pixel6-dtl",
        new UserFeedback(
            FeedbackClassification.AUDIO_VIDEO_SYNC_WRONG, "Audio \"late\"\nby eye"),
        List.of(
            mark(TimingMarkKind.AUDIO_IMPACT_TRANSIENT, "audio", 120),
            mark(TimingMarkKind.VISUAL_BALL_IMPACT, "high_speed", 0),
            mark(TimingMarkKind.HIGH_SPEED_SHOULD_START, "preview", -700_000)));
  }

  private static DiagnosticIncident unreviewedIncident() {
    return new DiagnosticIncident(
        "incident-17",
        IncidentClassification.SUCCESSFUL_CAPTURE,
        1_786_900_000_123L,
        "pixel6-dtl",
        UserFeedback.unreviewed(),
        List.of());
  }

  private static TimingMark mark(TimingMarkKind kind, String stream, long offsetMicros) {
    return new TimingMark(kind, stream, offsetMicros);
  }

  private static void expectBadRequest(String json, String message) {
    expectIllegalArgument(() -> DiagnosticFeedbackRequest.parseJson(json), message);
  }

  private static void expectIllegalArgument(Action action, String message) {
    try {
      action.run();
    } catch (IllegalArgumentException expected) {
      return;
    }
    throw new AssertionError(message + " was accepted");
  }

  private static void expectUnsupported(Action action, String message) {
    try {
      action.run();
    } catch (UnsupportedOperationException expected) {
      return;
    }
    throw new AssertionError(message + " was accepted");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }

  @FunctionalInterface
  private interface Action {
    void run();
  }
}
