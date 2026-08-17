package com.agoessling.swingcapture.diagnostics;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;

/** Strict schema-v1 contract for incident classification, user feedback, and timing labels. */
public record DiagnosticIncident(
    String incidentId,
    IncidentClassification classification,
    long createdAtEpochMillis,
    String sourceNodeId,
    UserFeedback userFeedback,
    List<TimingMark> timingMarks) {
  public static final int SCHEMA_VERSION = 1;
  public static final int MAXIMUM_SERIALIZED_BYTES = 64 * 1024;
  public static final int MAXIMUM_IDENTIFIER_BYTES = 128;
  public static final int MAXIMUM_NOTE_BYTES = 2_048;
  public static final int MAXIMUM_NOTE_CODE_UNITS = 500;
  public static final int MAXIMUM_TIMING_MARKS = 32;

  private static final Set<String> ROOT_FIELDS =
      Set.of(
          "schema_version",
          "incident_id",
          "classification",
          "created_at_epoch_ms",
          "source_node_id",
          "user_feedback",
          "timing_marks");
  private static final Set<String> FEEDBACK_FIELDS = Set.of("classification", "note");
  private static final Set<String> MARK_FIELDS = Set.of("kind", "stream_id", "offset_us");
  private static final Comparator<TimingMark> MARK_ORDER =
      Comparator.comparingLong(TimingMark::offsetMicros)
          .thenComparing(mark -> mark.kind().wireName())
          .thenComparing(TimingMark::streamId);

  public enum IncidentClassification {
    SUCCESSFUL_CAPTURE("successful_capture"),
    POSE_ARMED_NO_IMPACT("pose_armed_no_impact"),
    IMPACT_WHILE_NOT_ARMED("impact_while_not_armed"),
    AUDIO_TIMESTAMP_MAPPING_REJECTED("audio_timestamp_mapping_rejected"),
    ENCODER_CONTINUITY_FAILURE("encoder_continuity_failure"),
    PEER_FAILURE("peer_failure"),
    UNEXPECTED_AUDIO_TRIGGER("unexpected_audio_trigger"),
    USER_REPORTED("user_reported");

    private final String wireName;

    IncidentClassification(String wireName) {
      this.wireName = wireName;
    }

    public String wireName() {
      return wireName;
    }

    private static IncidentClassification parse(String value) {
      for (IncidentClassification classification : values()) {
        if (classification.wireName.equals(value)) {
          return classification;
        }
      }
      throw new IllegalArgumentException("unknown incident classification: " + value);
    }
  }

  public enum FeedbackClassification {
    UNREVIEWED("unreviewed"),
    GOOD_CAPTURE("good_capture"),
    ARMED_TOO_EARLY("armed_too_early"),
    ARMED_TOO_LATE("armed_too_late"),
    MISSED_SHOT("missed_shot"),
    FALSE_IMPACT("false_impact"),
    AUDIO_VIDEO_SYNC_WRONG("av_sync_wrong"),
    OTHER("other");

    private final String wireName;

    FeedbackClassification(String wireName) {
      this.wireName = wireName;
    }

    public String wireName() {
      return wireName;
    }

    private static FeedbackClassification parse(String value) {
      for (FeedbackClassification classification : values()) {
        if (classification.wireName.equals(value)) {
          return classification;
        }
      }
      throw new IllegalArgumentException("unknown feedback classification: " + value);
    }
  }

  public enum TimingMarkKind {
    HIGH_SPEED_SHOULD_START("high_speed_should_start"),
    VISUAL_BALL_IMPACT("visual_ball_impact"),
    AUDIO_IMPACT_TRANSIENT("audio_impact_transient");

    private final String wireName;

    TimingMarkKind(String wireName) {
      this.wireName = wireName;
    }

    public String wireName() {
      return wireName;
    }

    private static TimingMarkKind parse(String value) {
      for (TimingMarkKind kind : values()) {
        if (kind.wireName.equals(value)) {
          return kind;
        }
      }
      throw new IllegalArgumentException("unknown timing mark kind: " + value);
    }
  }

  /** Explicit review state; UNREVIEWED with an empty note represents no user feedback yet. */
  public record UserFeedback(FeedbackClassification classification, String note) {
    public UserFeedback {
      Objects.requireNonNull(classification, "classification");
      requireBoundedText(note, MAXIMUM_NOTE_BYTES, "feedback note", true);
      if (note.length() > MAXIMUM_NOTE_CODE_UNITS) {
        throw new IllegalArgumentException("feedback note exceeds 500 UTF-16 code units");
      }
      if (classification == FeedbackClassification.UNREVIEWED && !note.isEmpty()) {
        throw new IllegalArgumentException("unreviewed feedback cannot contain a note");
      }
    }

    public static UserFeedback unreviewed() {
      return new UserFeedback(FeedbackClassification.UNREVIEWED, "");
    }
  }

  /** A signed microsecond offset relative to detected impact in one diagnostic stream. */
  public record TimingMark(TimingMarkKind kind, String streamId, long offsetMicros) {
    public TimingMark {
      Objects.requireNonNull(kind, "kind");
      requireIdentifier(streamId, "streamId");
    }
  }

  public DiagnosticIncident {
    requireIdentifier(incidentId, "incidentId");
    Objects.requireNonNull(classification, "classification");
    if (createdAtEpochMillis <= 0) {
      throw new IllegalArgumentException("createdAtEpochMillis must be positive");
    }
    requireIdentifier(sourceNodeId, "sourceNodeId");
    Objects.requireNonNull(userFeedback, "userFeedback");
    Objects.requireNonNull(timingMarks, "timingMarks");
    if (timingMarks.size() > MAXIMUM_TIMING_MARKS) {
      throw new IllegalArgumentException("too many timing marks");
    }
    ArrayList<TimingMark> sorted = new ArrayList<>(timingMarks.size());
    for (TimingMark mark : timingMarks) {
      sorted.add(Objects.requireNonNull(mark, "timing mark"));
    }
    sorted.sort(MARK_ORDER);
    Set<String> identities = new HashSet<>();
    for (TimingMark mark : sorted) {
      String identity = mark.kind().wireName() + '\u0000' + mark.streamId();
      if (!identities.add(identity)) {
        throw new IllegalArgumentException("duplicate timing mark kind and stream");
      }
    }
    timingMarks = List.copyOf(sorted);
  }

  /** Canonical compact UTF-8 JSON with stable field and timing-mark ordering. */
  public String toCanonicalJson() {
    StringBuilder json = new StringBuilder(1_024);
    json.append('{');
    appendNumber(json, "schema_version", SCHEMA_VERSION);
    appendString(json, "incident_id", incidentId);
    appendString(json, "classification", classification.wireName());
    appendString(json, "created_at_epoch_ms", Long.toString(createdAtEpochMillis));
    appendString(json, "source_node_id", sourceNodeId);
    json.append(",\"user_feedback\":{");
    appendFirstString(json, "classification", userFeedback.classification().wireName());
    appendString(json, "note", userFeedback.note());
    json.append("},\"timing_marks\":[");
    for (int index = 0; index < timingMarks.size(); ++index) {
      if (index != 0) {
        json.append(',');
      }
      TimingMark mark = timingMarks.get(index);
      json.append('{');
      appendFirstString(json, "kind", mark.kind().wireName());
      appendString(json, "stream_id", mark.streamId());
      appendString(json, "offset_us", Long.toString(mark.offsetMicros()));
      json.append('}');
    }
    json.append("]}");
    String result = json.toString();
    requireSerializedBound(result);
    return result;
  }

  /** Parses only the exact canonical schema-v1 representation and revalidates all fields. */
  public static DiagnosticIncident fromCanonicalJson(String json) {
    Objects.requireNonNull(json, "json");
    requireSerializedBound(json);
    Map<String, Object> root = StrictJson.parseObject(json);
    requireFields(root, ROOT_FIELDS, "diagnostic incident");
    if (requiredLong(root, "schema_version") != SCHEMA_VERSION) {
      throw new IllegalArgumentException("unsupported diagnostic incident schema");
    }
    Map<String, Object> encodedFeedback = requiredObject(root, "user_feedback");
    requireFields(encodedFeedback, FEEDBACK_FIELDS, "user feedback");
    UserFeedback feedback =
        new UserFeedback(
            FeedbackClassification.parse(requiredString(encodedFeedback, "classification")),
            requiredString(encodedFeedback, "note"));
    List<Object> encodedMarks = requiredArray(root, "timing_marks");
    if (encodedMarks.size() > MAXIMUM_TIMING_MARKS) {
      throw new IllegalArgumentException("too many timing marks");
    }
    ArrayList<TimingMark> marks = new ArrayList<>(encodedMarks.size());
    for (Object encodedMark : encodedMarks) {
      if (!(encodedMark instanceof Map<?, ?> untyped)) {
        throw new IllegalArgumentException("timing mark must be an object");
      }
      Map<String, Object> mark = checkedObject(untyped, "timing mark");
      requireFields(mark, MARK_FIELDS, "timing mark");
      marks.add(
          new TimingMark(
              TimingMarkKind.parse(requiredString(mark, "kind")),
              requiredString(mark, "stream_id"),
              requiredDecimalLong(mark, "offset_us")));
    }
    DiagnosticIncident result =
        new DiagnosticIncident(
            requiredString(root, "incident_id"),
            IncidentClassification.parse(requiredString(root, "classification")),
            requiredDecimalLong(root, "created_at_epoch_ms"),
            requiredString(root, "source_node_id"),
            feedback,
            marks);
    if (!result.toCanonicalJson().equals(json)) {
      throw new IllegalArgumentException("diagnostic incident JSON is not canonical");
    }
    return result;
  }

  private static void appendNumber(StringBuilder json, String name, long value) {
    appendSeparatorAndName(json, name);
    json.append(value);
  }

  private static void appendFirstString(StringBuilder json, String name, String value) {
    appendEscapedString(json, name);
    json.append(':');
    appendEscapedString(json, value);
  }

  private static void appendString(StringBuilder json, String name, String value) {
    appendSeparatorAndName(json, name);
    appendEscapedString(json, value);
  }

  private static void appendSeparatorAndName(StringBuilder json, String name) {
    if (json.charAt(json.length() - 1) != '{') {
      json.append(',');
    }
    appendEscapedString(json, name);
    json.append(':');
  }

  private static void appendEscapedString(StringBuilder json, String value) {
    json.append('"');
    for (int index = 0; index < value.length(); ++index) {
      char character = value.charAt(index);
      switch (character) {
        case '"' -> json.append("\\\"");
        case '\\' -> json.append("\\\\");
        case '\b' -> json.append("\\b");
        case '\f' -> json.append("\\f");
        case '\n' -> json.append("\\n");
        case '\r' -> json.append("\\r");
        case '\t' -> json.append("\\t");
        default -> {
          if (character < 0x20) {
            json.append(String.format(java.util.Locale.ROOT, "\\u%04x", (int) character));
          } else {
            json.append(character);
          }
        }
      }
    }
    json.append('"');
  }

  private static void requireFields(
      Map<String, Object> object, Set<String> expected, String description) {
    if (!object.keySet().equals(expected)) {
      throw new IllegalArgumentException(description + " fields do not match schema");
    }
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

  private static long requiredDecimalLong(Map<String, Object> object, String field) {
    String value = requiredString(object, field);
    try {
      long parsed = Long.parseLong(value);
      if (!Long.toString(parsed).equals(value)) {
        throw new IllegalArgumentException(field + " must use canonical decimal notation");
      }
      return parsed;
    } catch (NumberFormatException failure) {
      throw new IllegalArgumentException(field + " exceeds signed 64-bit range", failure);
    }
  }

  private static Map<String, Object> requiredObject(
      Map<String, Object> object, String field) {
    Object value = object.get(field);
    if (!(value instanceof Map<?, ?> untyped)) {
      throw new IllegalArgumentException(field + " must be an object");
    }
    return checkedObject(untyped, field);
  }

  private static Map<String, Object> checkedObject(Map<?, ?> value, String description) {
    for (Map.Entry<?, ?> entry : value.entrySet()) {
      if (!(entry.getKey() instanceof String)) {
        throw new IllegalArgumentException(description + " field name must be a string");
      }
    }
    @SuppressWarnings("unchecked")
    Map<String, Object> checked = (Map<String, Object>) value;
    return checked;
  }

  private static List<Object> requiredArray(Map<String, Object> object, String field) {
    Object value = object.get(field);
    if (!(value instanceof List<?> untyped)) {
      throw new IllegalArgumentException(field + " must be an array");
    }
    @SuppressWarnings("unchecked")
    List<Object> checked = (List<Object>) untyped;
    return checked;
  }

  private static void requireIdentifier(String value, String name) {
    requireBoundedText(value, MAXIMUM_IDENTIFIER_BYTES, name, false);
    for (int index = 0; index < value.length(); ++index) {
      char character = value.charAt(index);
      if (!(character >= 'a' && character <= 'z')
          && !(character >= 'A' && character <= 'Z')
          && !(character >= '0' && character <= '9')
          && character != '-'
          && character != '_'
          && character != '.') {
        throw new IllegalArgumentException(name + " contains a prohibited character");
      }
    }
  }

  private static void requireBoundedText(
      String value, int maximumBytes, String name, boolean allowEmpty) {
    Objects.requireNonNull(value, name);
    if ((!allowEmpty && value.isEmpty())
        || value.getBytes(StandardCharsets.UTF_8).length > maximumBytes) {
      throw new IllegalArgumentException(name + " has an invalid UTF-8 length");
    }
    for (int index = 0; index < value.length(); ++index) {
      char character = value.charAt(index);
      if (Character.isSurrogate(character)) {
        if (!Character.isHighSurrogate(character)
            || index + 1 >= value.length()
            || !Character.isLowSurrogate(value.charAt(index + 1))) {
          throw new IllegalArgumentException(name + " contains an unpaired surrogate");
        }
        ++index;
      } else if (character < 0x20 && character != '\n' && character != '\r' && character != '\t') {
        throw new IllegalArgumentException(name + " contains a prohibited control character");
      }
    }
  }

  private static void requireSerializedBound(String json) {
    if (json.getBytes(StandardCharsets.UTF_8).length > MAXIMUM_SERIALIZED_BYTES) {
      throw new IllegalArgumentException("diagnostic incident exceeds serialized size limit");
    }
  }
}
