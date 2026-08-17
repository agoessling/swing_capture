package com.agoessling.swingcapture.diagnostics;

import com.agoessling.swingcapture.diagnostics.DiagnosticIncident.FeedbackClassification;
import com.agoessling.swingcapture.diagnostics.DiagnosticIncident.TimingMark;
import com.agoessling.swingcapture.diagnostics.DiagnosticIncident.TimingMarkKind;
import com.agoessling.swingcapture.diagnostics.DiagnosticIncident.UserFeedback;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;

/** Strict value and parser for the browser's schema-v1 diagnostic feedback request body. */
public record DiagnosticFeedbackRequest(
    FeedbackClassification classification,
    Optional<String> note,
    List<TimingMark> timingMarks) {
  public static final int SCHEMA_VERSION = 1;
  public static final int MAXIMUM_REQUEST_BYTES = 8 * 1024;

  public static final String DESIRED_HIGH_SPEED_START_FIELD = "desired_high_speed_start_us";
  public static final String VISUAL_IMPACT_FIELD = "visual_impact_us";
  public static final String AUDIO_IMPACT_FIELD = "audio_impact_us";

  private static final Set<String> REQUIRED_FIELDS = Set.of("schema_version", "classification");
  private static final Set<String> ALLOWED_FIELDS =
      Set.of("schema_version", "classification", "note", "timing_marks_us");
  private static final Set<String> ALLOWED_TIMING_FIELDS =
      Set.of(DESIRED_HIGH_SPEED_START_FIELD, VISUAL_IMPACT_FIELD, AUDIO_IMPACT_FIELD);

  public DiagnosticFeedbackRequest {
    Objects.requireNonNull(classification, "classification");
    if (classification == FeedbackClassification.UNREVIEWED) {
      throw new IllegalArgumentException("feedback request classification cannot be unreviewed");
    }
    Objects.requireNonNull(note, "note");
    note.ifPresent(value -> new UserFeedback(classification, value));
    Objects.requireNonNull(timingMarks, "timingMarks");
    ArrayList<TimingMark> checked = new ArrayList<>(timingMarks.size());
    for (TimingMark mark : timingMarks) {
      TimingMark nonNull = Objects.requireNonNull(mark, "timing mark");
      requireRequestMark(nonNull);
      checked.add(nonNull);
    }
    if (checked.size() > 3) {
      throw new IllegalArgumentException("feedback request contains too many timing marks");
    }
    DiagnosticIncident validation =
        new DiagnosticIncident(
            "validation",
            DiagnosticIncident.IncidentClassification.USER_REPORTED,
            1,
            "validation",
            new UserFeedback(classification, note.orElse("")),
            checked);
    timingMarks = validation.timingMarks();
    note = note.map(String::toString);
  }

  /** Parses the exact request schema, rejecting unknown, duplicate, or mistyped fields. */
  public static DiagnosticFeedbackRequest parseJson(String json) {
    Objects.requireNonNull(json, "json");
    if (json.getBytes(StandardCharsets.UTF_8).length > MAXIMUM_REQUEST_BYTES) {
      throw new IllegalArgumentException("diagnostic feedback request exceeds size limit");
    }
    Map<String, Object> root = StrictJson.parseObject(json);
    if (!root.keySet().containsAll(REQUIRED_FIELDS) || !ALLOWED_FIELDS.containsAll(root.keySet())) {
      throw new IllegalArgumentException("diagnostic feedback request fields do not match schema");
    }
    if (requiredLong(root, "schema_version") != SCHEMA_VERSION) {
      throw new IllegalArgumentException("unsupported diagnostic feedback request schema");
    }
    FeedbackClassification classification = parseFeedback(requiredString(root, "classification"));
    Optional<String> note =
        root.containsKey("note") ? Optional.of(requiredString(root, "note")) : Optional.empty();
    ArrayList<TimingMark> marks = new ArrayList<>();
    if (root.containsKey("timing_marks_us")) {
      Map<String, Object> timing = requiredObject(root, "timing_marks_us");
      if (!ALLOWED_TIMING_FIELDS.containsAll(timing.keySet())) {
        throw new IllegalArgumentException("diagnostic timing mark fields do not match schema");
      }
      addMarkIfPresent(
          marks,
          timing,
          DESIRED_HIGH_SPEED_START_FIELD,
          TimingMarkKind.HIGH_SPEED_SHOULD_START,
          "preview");
      addMarkIfPresent(
          marks,
          timing,
          VISUAL_IMPACT_FIELD,
          TimingMarkKind.VISUAL_BALL_IMPACT,
          "high_speed");
      addMarkIfPresent(
          marks,
          timing,
          AUDIO_IMPACT_FIELD,
          TimingMarkKind.AUDIO_IMPACT_TRANSIENT,
          "audio");
    }
    return new DiagnosticFeedbackRequest(classification, note, marks);
  }

  /** Returns a copy of an existing incident carrying this review and these user timing labels. */
  public DiagnosticIncident applyTo(DiagnosticIncident incident) {
    Objects.requireNonNull(incident, "incident");
    return new DiagnosticIncident(
        incident.incidentId(),
        incident.classification(),
        incident.createdAtEpochMillis(),
        incident.sourceNodeId(),
        new UserFeedback(classification, note.orElse("")),
        timingMarks);
  }

  private static void addMarkIfPresent(
      List<TimingMark> marks,
      Map<String, Object> encoded,
      String field,
      TimingMarkKind kind,
      String streamId) {
    if (encoded.containsKey(field)) {
      marks.add(new TimingMark(kind, streamId, requiredLong(encoded, field)));
    }
  }

  private static void requireRequestMark(TimingMark mark) {
    boolean valid =
        switch (mark.kind()) {
          case HIGH_SPEED_SHOULD_START -> mark.streamId().equals("preview");
          case VISUAL_BALL_IMPACT -> mark.streamId().equals("high_speed");
          case AUDIO_IMPACT_TRANSIENT -> mark.streamId().equals("audio");
        };
    if (!valid) {
      throw new IllegalArgumentException("feedback timing mark uses the wrong diagnostic stream");
    }
  }

  private static FeedbackClassification parseFeedback(String value) {
    for (FeedbackClassification classification : FeedbackClassification.values()) {
      if (classification != FeedbackClassification.UNREVIEWED
          && classification.wireName().equals(value)) {
        return classification;
      }
    }
    throw new IllegalArgumentException("unknown feedback classification: " + value);
  }

  private static String requiredString(Map<String, Object> object, String field) {
    Object value = object.get(field);
    if (!(value instanceof String result)) {
      throw new IllegalArgumentException(field + " must be a string");
    }
    return result;
  }

  private static long requiredLong(Map<String, Object> object, String field) {
    Object value = object.get(field);
    if (!(value instanceof Long result)) {
      throw new IllegalArgumentException(field + " must be an integer");
    }
    return result;
  }

  private static Map<String, Object> requiredObject(
      Map<String, Object> object, String field) {
    Object value = object.get(field);
    if (!(value instanceof Map<?, ?> untyped)) {
      throw new IllegalArgumentException(field + " must be an object");
    }
    for (Map.Entry<?, ?> entry : untyped.entrySet()) {
      if (!(entry.getKey() instanceof String)) {
        throw new IllegalArgumentException(field + " field name must be a string");
      }
    }
    @SuppressWarnings("unchecked")
    Map<String, Object> checked = (Map<String, Object>) untyped;
    return checked;
  }
}
