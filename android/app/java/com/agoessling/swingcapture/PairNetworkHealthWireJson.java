package com.agoessling.swingcapture;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;

/** Host-testable strict JSON codec for the authenticated reverse-probe wire contract. */
final class PairNetworkHealthWireJson {
  static final int MAXIMUM_REVERSE_REQUEST_BYTES = 16 * 1024;
  static final int MAXIMUM_DIRECTION_BYTES = 64 * 1024;

  private PairNetworkHealthWireJson() {}

  static byte[] reverseRequest(String callbackOrigin, String callbackNodeId) throws IOException {
    if (callbackOrigin == null
        || callbackOrigin.isBlank()
        || callbackNodeId == null
        || callbackNodeId.isBlank()) {
      throw new IllegalArgumentException("reverse callback origin and node ID cannot be blank");
    }
    return boundedUtf8(
        "{\"schema_version\":1,\"callback_origin\":"
            + quote(callbackOrigin)
            + ",\"callback_node_id\":"
            + quote(callbackNodeId)
            + "}",
        MAXIMUM_REVERSE_REQUEST_BYTES,
        "reverse network-health request");
  }

  static ReverseRequest parseReverseRequest(byte[] contents) throws IOException {
    Map<String, Object> object = parseObject(contents, MAXIMUM_REVERSE_REQUEST_BYTES, "request");
    requireExactFields(object, Set.of("schema_version", "callback_origin", "callback_node_id"));
    if (strictInt(object, "schema_version") != 1) {
      throw malformed("schema_version must be 1");
    }
    return new ReverseRequest(
        strictNonemptyString(object, "callback_origin"),
        strictNonemptyString(object, "callback_node_id"));
  }

  static byte[] directionBytes(PairNetworkHealthPolicy.DirectionEvidence direction)
      throws IOException {
    Objects.requireNonNull(direction, "direction");
    StringBuilder roundTrips = new StringBuilder("[");
    for (int index = 0; index < direction.roundTripNanos().size(); ++index) {
      if (index != 0) {
        roundTrips.append(',');
      }
      roundTrips.append(quote(Long.toString(direction.roundTripNanos().get(index))));
    }
    roundTrips.append(']');
    return boundedUtf8(
        "{\"schema_version\":1,\"attempts\":"
            + direction.attempts()
            + ",\"successes\":"
            + direction.successes()
            + ",\"timeouts\":"
            + direction.timeouts()
            + ",\"round_trip_ns\":"
            + roundTrips
            + ",\"transfer_bytes\":"
            + quote(Long.toString(direction.transferBytes()))
            + ",\"transfer_duration_ns\":"
            + quote(Long.toString(direction.transferDurationNanos()))
            + ",\"transfer_complete\":"
            + direction.transferComplete()
            + "}",
        MAXIMUM_DIRECTION_BYTES,
        "reverse network-health response");
  }

  static PairNetworkHealthPolicy.DirectionEvidence parseDirection(byte[] contents)
      throws IOException {
    Map<String, Object> object = parseObject(contents, MAXIMUM_DIRECTION_BYTES, "response");
    requireExactFields(
        object,
        Set.of(
            "schema_version",
            "attempts",
            "successes",
            "timeouts",
            "round_trip_ns",
            "transfer_bytes",
            "transfer_duration_ns",
            "transfer_complete"));
    if (strictInt(object, "schema_version") != 1) {
      throw malformed("schema_version must be 1");
    }
    Object roundTripValue = object.get("round_trip_ns");
    if (!(roundTripValue instanceof List<?> roundTrips)) {
      throw malformed("round_trip_ns must be an array");
    }
    ArrayList<Long> parsedRoundTrips = new ArrayList<>();
    for (Object value : roundTrips) {
      if (!(value instanceof String text)) {
        throw malformed("round_trip_ns must contain integer strings");
      }
      parsedRoundTrips.add(parseNonnegativeLong(text, "round_trip_ns"));
    }
    try {
      return new PairNetworkHealthPolicy.DirectionEvidence(
          strictInt(object, "attempts"),
          strictInt(object, "successes"),
          strictInt(object, "timeouts"),
          parsedRoundTrips,
          strictLongString(object, "transfer_bytes"),
          strictLongString(object, "transfer_duration_ns"),
          strictBoolean(object, "transfer_complete"));
    } catch (IllegalArgumentException invalidEvidence) {
      throw new IOException("invalid reverse network-health response", invalidEvidence);
    }
  }

  private static Map<String, Object> parseObject(byte[] contents, int maximumBytes, String kind)
      throws IOException {
    Objects.requireNonNull(contents, "contents");
    if (contents.length == 0 || contents.length > maximumBytes) {
      throw new IOException("invalid reverse network-health " + kind + " length");
    }
    String text;
    try {
      text =
          StandardCharsets.UTF_8
              .newDecoder()
              .onMalformedInput(CodingErrorAction.REPORT)
              .onUnmappableCharacter(CodingErrorAction.REPORT)
              .decode(ByteBuffer.wrap(contents))
              .toString();
    } catch (CharacterCodingException invalidUtf8) {
      throw new IOException("invalid reverse network-health " + kind + " UTF-8", invalidUtf8);
    }
    try {
      Object value = new Parser(text).parseDocument();
      if (!(value instanceof Map<?, ?> rawObject)) {
        throw new IllegalArgumentException("top-level value must be an object");
      }
      LinkedHashMap<String, Object> object = new LinkedHashMap<>();
      for (Map.Entry<?, ?> entry : rawObject.entrySet()) {
        object.put((String) entry.getKey(), entry.getValue());
      }
      return object;
    } catch (IllegalArgumentException malformed) {
      throw new IOException("invalid reverse network-health " + kind, malformed);
    }
  }

  private static byte[] boundedUtf8(String text, int maximumBytes, String label)
      throws IOException {
    byte[] result = text.getBytes(StandardCharsets.UTF_8);
    if (result.length > maximumBytes) {
      throw new IOException(label + " exceeds configured bound");
    }
    return result;
  }

  private static String quote(String text) {
    StringBuilder result = new StringBuilder(text.length() + 2).append('"');
    for (int index = 0; index < text.length(); ++index) {
      char character = text.charAt(index);
      switch (character) {
        case '"' -> result.append("\\\"");
        case '\\' -> result.append("\\\\");
        case '\b' -> result.append("\\b");
        case '\f' -> result.append("\\f");
        case '\n' -> result.append("\\n");
        case '\r' -> result.append("\\r");
        case '\t' -> result.append("\\t");
        default -> {
          if (character < 0x20) {
            result.append(String.format("\\u%04x", (int) character));
          } else {
            result.append(character);
          }
        }
      }
    }
    return result.append('"').toString();
  }

  private static void requireExactFields(Map<String, Object> object, Set<String> fields)
      throws IOException {
    if (!object.keySet().equals(fields)) {
      throw malformed("JSON fields do not match the network-health schema");
    }
  }

  private static int strictInt(Map<String, Object> object, String field) throws IOException {
    Object value = object.get(field);
    if (!(value instanceof Long number)
        || number < Integer.MIN_VALUE
        || number > Integer.MAX_VALUE) {
      throw malformed(field + " must be a JSON integer");
    }
    return number.intValue();
  }

  private static boolean strictBoolean(Map<String, Object> object, String field)
      throws IOException {
    Object value = object.get(field);
    if (!(value instanceof Boolean result)) {
      throw malformed(field + " must be a boolean");
    }
    return result;
  }

  private static String strictNonemptyString(Map<String, Object> object, String field)
      throws IOException {
    Object value = object.get(field);
    if (!(value instanceof String text) || text.isBlank()) {
      throw malformed(field + " must be a nonempty string");
    }
    return text;
  }

  private static long strictLongString(Map<String, Object> object, String field)
      throws IOException {
    Object value = object.get(field);
    if (!(value instanceof String text)) {
      throw malformed(field + " must be an integer string");
    }
    return parseNonnegativeLong(text, field);
  }

  private static long parseNonnegativeLong(String text, String field) throws IOException {
    if (!text.matches("0|[1-9][0-9]*")) {
      throw malformed(field + " must be a canonical nonnegative integer");
    }
    try {
      return Long.parseLong(text);
    } catch (NumberFormatException overflow) {
      throw new IOException(field + " is outside the signed 64-bit range", overflow);
    }
  }

  private static IOException malformed(String detail) {
    return new IOException(detail);
  }

  record ReverseRequest(String callbackOrigin, String callbackNodeId) {}

  /** Minimal strict JSON parser: duplicate fields, trailing data, floats, and invalid escapes fail. */
  private static final class Parser {
    private final String text;
    private int cursor;

    private Parser(String text) {
      this.text = text;
    }

    Object parseDocument() {
      Object value = parseValue();
      whitespace();
      if (cursor != text.length()) {
        throw invalid();
      }
      return value;
    }

    private Object parseValue() {
      whitespace();
      if (cursor >= text.length()) {
        throw invalid();
      }
      return switch (text.charAt(cursor)) {
        case '{' -> parseObject();
        case '[' -> parseArray();
        case '"' -> parseString();
        case 't' -> parseLiteral("true", true);
        case 'f' -> parseLiteral("false", false);
        case 'n' -> parseLiteral("null", null);
        default -> parseInteger();
      };
    }

    private Map<String, Object> parseObject() {
      ++cursor;
      whitespace();
      LinkedHashMap<String, Object> result = new LinkedHashMap<>();
      if (consume('}')) {
        return result;
      }
      while (true) {
        whitespace();
        if (cursor >= text.length() || text.charAt(cursor) != '"') {
          throw invalid();
        }
        String key = parseString();
        whitespace();
        require(':');
        Object value = parseValue();
        if (result.containsKey(key)) {
          throw new IllegalArgumentException("duplicate JSON field");
        }
        result.put(key, value);
        whitespace();
        if (consume('}')) {
          return result;
        }
        require(',');
      }
    }

    private List<Object> parseArray() {
      ++cursor;
      whitespace();
      ArrayList<Object> result = new ArrayList<>();
      if (consume(']')) {
        return result;
      }
      while (true) {
        result.add(parseValue());
        whitespace();
        if (consume(']')) {
          return result;
        }
        require(',');
      }
    }

    private String parseString() {
      require('"');
      StringBuilder result = new StringBuilder();
      while (cursor < text.length()) {
        char character = text.charAt(cursor++);
        if (character == '"') {
          return result.toString();
        }
        if (character < 0x20) {
          throw invalid();
        }
        if (character != '\\') {
          result.append(character);
          continue;
        }
        if (cursor >= text.length()) {
          throw invalid();
        }
        char escape = text.charAt(cursor++);
        switch (escape) {
          case '"', '\\', '/' -> result.append(escape);
          case 'b' -> result.append('\b');
          case 'f' -> result.append('\f');
          case 'n' -> result.append('\n');
          case 'r' -> result.append('\r');
          case 't' -> result.append('\t');
          case 'u' -> result.append(parseUnicodeEscape());
          default -> throw invalid();
        }
      }
      throw invalid();
    }

    private char parseUnicodeEscape() {
      if (cursor + 4 > text.length()) {
        throw invalid();
      }
      int value = 0;
      for (int index = 0; index < 4; ++index) {
        int digit = Character.digit(text.charAt(cursor++), 16);
        if (digit < 0) {
          throw invalid();
        }
        value = value * 16 + digit;
      }
      return (char) value;
    }

    private Object parseLiteral(String literal, Object value) {
      if (!text.startsWith(literal, cursor)) {
        throw invalid();
      }
      cursor += literal.length();
      return value;
    }

    private Long parseInteger() {
      int beginning = cursor;
      consume('-');
      if (consume('0')) {
        if (cursor < text.length() && Character.isDigit(text.charAt(cursor))) {
          throw invalid();
        }
      } else {
        int digits = cursor;
        while (cursor < text.length() && Character.isDigit(text.charAt(cursor))) {
          ++cursor;
        }
        if (digits == cursor) {
          throw invalid();
        }
      }
      if (cursor < text.length()
          && (text.charAt(cursor) == '.'
              || text.charAt(cursor) == 'e'
              || text.charAt(cursor) == 'E')) {
        throw invalid();
      }
      try {
        return Long.parseLong(text.substring(beginning, cursor));
      } catch (NumberFormatException invalidNumber) {
        throw invalid();
      }
    }

    private void whitespace() {
      while (cursor < text.length()) {
        char character = text.charAt(cursor);
        if (character != ' ' && character != '\n' && character != '\r' && character != '\t') {
          return;
        }
        ++cursor;
      }
    }

    private boolean consume(char expected) {
      if (cursor < text.length() && text.charAt(cursor) == expected) {
        ++cursor;
        return true;
      }
      return false;
    }

    private void require(char expected) {
      if (!consume(expected)) {
        throw invalid();
      }
    }

    private IllegalArgumentException invalid() {
      return new IllegalArgumentException("malformed JSON at character " + cursor);
    }
  }
}
