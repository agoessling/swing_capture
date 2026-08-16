package com.agoessling.swingcapture.core.coordination;

import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.Objects;
import java.util.Set;

/** Immutable, bounded evidence proving one shared session contains exactly two mapped triggers. */
public record PairedCoordinationRecord(
    String sharedSessionId,
    long recordedAtEpochMillis,
    NodeEvidence downTheLine,
    NodeEvidence faceOn,
    long minimumTriggerSeparationNs,
    long maximumTriggerSeparationNs) {
  public static final int SCHEMA_VERSION = 1;
  public static final int MAXIMUM_SERIALIZED_BYTES = 32 * 1024;
  public static final int MAXIMUM_IDENTIFIER_LENGTH = 128;
  public static final int MAXIMUM_SOURCE_LENGTH = 64;
  public static final int MAXIMUM_CLOCK_SAMPLE_COUNT = 64;

  private static final Set<String> ROOT_FIELDS =
      Set.of(
          "schema_version",
          "shared_session_id",
          "status",
          "recorded_at_epoch_ms",
          "down_the_line",
          "face_on",
          "minimum_trigger_separation_ns",
          "maximum_trigger_separation_ns");
  private static final Set<String> NODE_FIELDS =
      Set.of(
          "role",
          "node_id",
          "local_session_id",
          "trigger_timestamp_ns",
          "trigger_uncertainty_ns",
          "mapped_coordinator_timestamp_ns",
          "mapped_coordinator_uncertainty_ns",
          "clock_offset_ns",
          "clock_uncertainty_ns",
          "minimum_round_trip_ns",
          "maximum_round_trip_ns",
          "clock_sample_count",
          "source");

  /** Complete trigger and clock-exchange evidence for one phone. */
  public record NodeEvidence(
      CaptureRole role,
      String nodeId,
      String localSessionId,
      long triggerTimestampNs,
      long triggerUncertaintyNs,
      long mappedCoordinatorTimestampNs,
      long mappedCoordinatorUncertaintyNs,
      long clockOffsetNs,
      long clockUncertaintyNs,
      long minimumRoundTripNs,
      long maximumRoundTripNs,
      int clockSampleCount,
      String source) {
    public NodeEvidence {
      Objects.requireNonNull(role, "role");
      requireIdentifier(nodeId, "nodeId");
      requireIdentifier(localSessionId, "localSessionId");
      requireToken(source, MAXIMUM_SOURCE_LENGTH, "source");
      if (triggerTimestampNs <= 0) {
        throw new IllegalArgumentException("triggerTimestampNs must be positive");
      }
      if (triggerUncertaintyNs < 0
          || mappedCoordinatorTimestampNs < 0
          || mappedCoordinatorUncertaintyNs < 0
          || clockUncertaintyNs < 0
          || minimumRoundTripNs < 0
          || maximumRoundTripNs < minimumRoundTripNs) {
        throw new IllegalArgumentException("mapped trigger uncertainty and RTT bounds are invalid");
      }
      if (clockSampleCount < 1 || clockSampleCount > MAXIMUM_CLOCK_SAMPLE_COUNT) {
        throw new IllegalArgumentException("clockSampleCount must be between 1 and 64");
      }
      long expectedMappedTimestamp = Math.subtractExact(triggerTimestampNs, clockOffsetNs);
      if (mappedCoordinatorTimestampNs != expectedMappedTimestamp) {
        throw new IllegalArgumentException("mapped timestamp disagrees with the clock offset");
      }
      long expectedMappedUncertainty =
          Math.addExact(triggerUncertaintyNs, clockUncertaintyNs);
      if (mappedCoordinatorUncertaintyNs != expectedMappedUncertainty) {
        throw new IllegalArgumentException(
            "mapped uncertainty must include trigger and clock uncertainty");
      }
    }

    /** Maps reusable coordinator primitives into durable per-node evidence. */
    public static NodeEvidence map(
        String sharedSessionId,
        NodeTriggerReport report,
        String localSessionId,
        String source,
        ClockOffsetEstimate clockEstimate) {
      requireIdentifier(sharedSessionId, "sharedSessionId");
      Objects.requireNonNull(report, "report");
      Objects.requireNonNull(clockEstimate, "clockEstimate");
      if (!sharedSessionId.equals(report.sessionId())) {
        throw new IllegalArgumentException("trigger report belongs to another shared session");
      }
      if (!report.nodeId().equals(clockEstimate.nodeId())) {
        throw new IllegalArgumentException("clock estimate belongs to a different node");
      }
      if (clockEstimate.sampleCount() < 1) {
        throw new IllegalArgumentException("durable evidence requires clock-exchange samples");
      }
      long mappedTimestamp =
          Math.subtractExact(report.triggerTimestampNs(), clockEstimate.offsetNs());
      long mappedUncertainty =
          Math.addExact(report.timestampUncertaintyNs(), clockEstimate.uncertaintyNs());
      return new NodeEvidence(
          report.role(),
          report.nodeId(),
          localSessionId,
          report.triggerTimestampNs(),
          report.timestampUncertaintyNs(),
          mappedTimestamp,
          mappedUncertainty,
          clockEstimate.offsetNs(),
          clockEstimate.uncertaintyNs(),
          clockEstimate.minimumRoundTripNs(),
          clockEstimate.maximumRoundTripNs(),
          clockEstimate.sampleCount(),
          source);
    }
  }

  public PairedCoordinationRecord {
    requireIdentifier(sharedSessionId, "sharedSessionId");
    if (recordedAtEpochMillis <= 0) {
      throw new IllegalArgumentException("recordedAtEpochMillis must be positive");
    }
    Objects.requireNonNull(downTheLine, "downTheLine");
    Objects.requireNonNull(faceOn, "faceOn");
    if (downTheLine.role() != CaptureRole.DOWN_THE_LINE
        || faceOn.role() != CaptureRole.FACE_ON) {
      throw new IllegalArgumentException("paired evidence must contain one node for each role");
    }
    if (downTheLine.nodeId().equals(faceOn.nodeId())) {
      throw new IllegalArgumentException("one node cannot provide both capture roles");
    }
    long expectedMinimum = minimumSeparation(downTheLine, faceOn);
    long expectedMaximum = maximumSeparation(downTheLine, faceOn);
    if (minimumTriggerSeparationNs != expectedMinimum
        || maximumTriggerSeparationNs != expectedMaximum) {
      throw new IllegalArgumentException(
          "stored trigger-separation bounds disagree with the mapped evidence");
    }
  }

  /** Creates a record and derives its conservative trigger-separation interval. */
  public static PairedCoordinationRecord create(
      String sharedSessionId,
      long recordedAtEpochMillis,
      NodeEvidence downTheLine,
      NodeEvidence faceOn) {
    return new PairedCoordinationRecord(
        sharedSessionId,
        recordedAtEpochMillis,
        downTheLine,
        faceOn,
        minimumSeparation(downTheLine, faceOn),
        maximumSeparation(downTheLine, faceOn));
  }

  /** Canonical schema-v1 JSON. Nanosecond values are decimal strings to preserve 64-bit integers. */
  public String toJson() {
    StringBuilder json = new StringBuilder(1_024);
    json.append('{');
    appendNumber(json, "schema_version", SCHEMA_VERSION);
    appendString(json, "shared_session_id", sharedSessionId);
    appendString(json, "status", "paired");
    appendString(json, "recorded_at_epoch_ms", Long.toString(recordedAtEpochMillis));
    appendNode(json, "down_the_line", downTheLine);
    appendNode(json, "face_on", faceOn);
    appendString(
        json, "minimum_trigger_separation_ns", Long.toString(minimumTriggerSeparationNs));
    appendString(
        json, "maximum_trigger_separation_ns", Long.toString(maximumTriggerSeparationNs));
    json.append('}');
    String value = json.toString();
    requireSerializedBound(value);
    return value;
  }

  /** Parses strict schema-v1 JSON and revalidates every derived timing invariant. */
  public static PairedCoordinationRecord fromJson(String json) {
    Objects.requireNonNull(json, "json");
    requireSerializedBound(json);
    Map<String, Object> root = StrictJson.parseObject(json);
    requireFields(root, ROOT_FIELDS, "coordination record");
    if (requiredLong(root, "schema_version") != SCHEMA_VERSION) {
      throw new IllegalArgumentException("unsupported coordination record schema");
    }
    if (!requiredString(root, "status").equals("paired")) {
      throw new IllegalArgumentException("coordination record status must be paired");
    }
    return new PairedCoordinationRecord(
        requiredString(root, "shared_session_id"),
        requiredDecimalLong(root, "recorded_at_epoch_ms"),
        parseNode(requiredObject(root, "down_the_line"), CaptureRole.DOWN_THE_LINE),
        parseNode(requiredObject(root, "face_on"), CaptureRole.FACE_ON),
        requiredDecimalLong(root, "minimum_trigger_separation_ns"),
        requiredDecimalLong(root, "maximum_trigger_separation_ns"));
  }

  /** Shared validation for storage keys and the serialized root identifier. */
  public static void validateSharedSessionId(String value) {
    requireIdentifier(value, "sharedSessionId");
  }

  private static NodeEvidence parseNode(Map<String, Object> value, CaptureRole expectedRole) {
    requireFields(value, NODE_FIELDS, expectedRole.wireName() + " evidence");
    CaptureRole role = CaptureRole.parse(requiredString(value, "role"));
    if (role != expectedRole) {
      throw new IllegalArgumentException("node evidence is stored under the wrong role");
    }
    return new NodeEvidence(
        role,
        requiredString(value, "node_id"),
        requiredString(value, "local_session_id"),
        requiredDecimalLong(value, "trigger_timestamp_ns"),
        requiredDecimalLong(value, "trigger_uncertainty_ns"),
        requiredDecimalLong(value, "mapped_coordinator_timestamp_ns"),
        requiredDecimalLong(value, "mapped_coordinator_uncertainty_ns"),
        requiredDecimalLong(value, "clock_offset_ns"),
        requiredDecimalLong(value, "clock_uncertainty_ns"),
        requiredDecimalLong(value, "minimum_round_trip_ns"),
        requiredDecimalLong(value, "maximum_round_trip_ns"),
        Math.toIntExact(requiredLong(value, "clock_sample_count")),
        requiredString(value, "source"));
  }

  private static long minimumSeparation(NodeEvidence first, NodeEvidence second) {
    long center =
        absoluteDifference(
            first.mappedCoordinatorTimestampNs(), second.mappedCoordinatorTimestampNs());
    long uncertainty =
        Math.addExact(
            first.mappedCoordinatorUncertaintyNs(), second.mappedCoordinatorUncertaintyNs());
    return center > uncertainty ? center - uncertainty : 0;
  }

  private static long maximumSeparation(NodeEvidence first, NodeEvidence second) {
    long center =
        absoluteDifference(
            first.mappedCoordinatorTimestampNs(), second.mappedCoordinatorTimestampNs());
    long uncertainty =
        Math.addExact(
            first.mappedCoordinatorUncertaintyNs(), second.mappedCoordinatorUncertaintyNs());
    return Math.addExact(center, uncertainty);
  }

  private static long absoluteDifference(long first, long second) {
    long difference = Math.subtractExact(first, second);
    if (difference == Long.MIN_VALUE) {
      throw new ArithmeticException("trigger separation overflows long");
    }
    return Math.abs(difference);
  }

  private static void appendNode(StringBuilder json, String name, NodeEvidence evidence) {
    appendName(json, name);
    json.append('{');
    appendString(json, "role", evidence.role().wireName());
    appendString(json, "node_id", evidence.nodeId());
    appendString(json, "local_session_id", evidence.localSessionId());
    appendString(json, "trigger_timestamp_ns", Long.toString(evidence.triggerTimestampNs()));
    appendString(json, "trigger_uncertainty_ns", Long.toString(evidence.triggerUncertaintyNs()));
    appendString(
        json,
        "mapped_coordinator_timestamp_ns",
        Long.toString(evidence.mappedCoordinatorTimestampNs()));
    appendString(
        json,
        "mapped_coordinator_uncertainty_ns",
        Long.toString(evidence.mappedCoordinatorUncertaintyNs()));
    appendString(json, "clock_offset_ns", Long.toString(evidence.clockOffsetNs()));
    appendString(json, "clock_uncertainty_ns", Long.toString(evidence.clockUncertaintyNs()));
    appendString(json, "minimum_round_trip_ns", Long.toString(evidence.minimumRoundTripNs()));
    appendString(json, "maximum_round_trip_ns", Long.toString(evidence.maximumRoundTripNs()));
    appendNumber(json, "clock_sample_count", evidence.clockSampleCount());
    appendString(json, "source", evidence.source());
    json.append('}');
  }

  private static void appendNumber(StringBuilder json, String name, long value) {
    appendName(json, name);
    json.append(value);
  }

  private static void appendString(StringBuilder json, String name, String value) {
    appendName(json, name);
    appendQuoted(json, value);
  }

  private static void appendName(StringBuilder json, String name) {
    if (json.charAt(json.length() - 1) != '{') {
      json.append(',');
    }
    appendQuoted(json, name);
    json.append(':');
  }

  private static void appendQuoted(StringBuilder json, String value) {
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
            json.append(String.format("\\u%04x", (int) character));
          } else {
            json.append(character);
          }
        }
      }
    }
    json.append('"');
  }

  private static void requireFields(
      Map<String, Object> object, Set<String> expectedFields, String label) {
    if (!object.keySet().equals(expectedFields)) {
      throw new IllegalArgumentException(label + " fields do not match schema v1");
    }
  }

  @SuppressWarnings("unchecked")
  private static Map<String, Object> requiredObject(Map<String, Object> object, String name) {
    Object value = object.get(name);
    if (!(value instanceof Map<?, ?>)) {
      throw new IllegalArgumentException(name + " must be an object");
    }
    return (Map<String, Object>) value;
  }

  private static String requiredString(Map<String, Object> object, String name) {
    Object value = object.get(name);
    if (!(value instanceof String string)) {
      throw new IllegalArgumentException(name + " must be a string");
    }
    return string;
  }

  private static long requiredLong(Map<String, Object> object, String name) {
    Object value = object.get(name);
    if (!(value instanceof Long number)) {
      throw new IllegalArgumentException(name + " must be an integer");
    }
    return number;
  }

  private static long requiredDecimalLong(Map<String, Object> object, String name) {
    String value = requiredString(object, name);
    if (!value.matches("-?(0|[1-9][0-9]*)") || value.equals("-0")) {
      throw new IllegalArgumentException(name + " must be a canonical decimal integer string");
    }
    try {
      return Long.parseLong(value);
    } catch (NumberFormatException failure) {
      throw new IllegalArgumentException(name + " exceeds signed 64-bit range", failure);
    }
  }

  private static void requireIdentifier(String value, String name) {
    requireToken(value, MAXIMUM_IDENTIFIER_LENGTH, name);
  }

  private static void requireToken(String value, int maximumLength, String name) {
    Objects.requireNonNull(value, name);
    if (value.isEmpty() || value.length() > maximumLength) {
      throw new IllegalArgumentException(name + " length is invalid");
    }
    for (int index = 0; index < value.length(); ++index) {
      char character = value.charAt(index);
      boolean valid =
          (character >= 'a' && character <= 'z')
              || (character >= 'A' && character <= 'Z')
              || (character >= '0' && character <= '9')
              || character == '.'
              || character == '_'
              || character == '-';
      if (!valid) {
        throw new IllegalArgumentException(name + " contains a non-identifier character");
      }
    }
  }

  private static void requireSerializedBound(String json) {
    if (json.getBytes(StandardCharsets.UTF_8).length > MAXIMUM_SERIALIZED_BYTES) {
      throw new IllegalArgumentException("coordination record exceeds the serialized size limit");
    }
  }
}
