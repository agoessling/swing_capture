package com.agoessling.swingcapture.core.coordination;

import java.util.LinkedHashMap;
import java.util.Map;

/** Minimal strict JSON parser for the fixed coordination-record schema. */
final class StrictJson {
  private static final int MAXIMUM_NESTING_DEPTH = 8;

  private final String input;
  private int cursor;

  private StrictJson(String input) {
    this.input = input;
  }

  static Map<String, Object> parseObject(String input) {
    StrictJson parser = new StrictJson(input);
    parser.skipWhitespace();
    Map<String, Object> result = parser.readObject(0);
    parser.skipWhitespace();
    if (parser.cursor != input.length()) {
      throw parser.error("unexpected trailing JSON content");
    }
    return result;
  }

  private Map<String, Object> readObject(int depth) {
    if (depth >= MAXIMUM_NESTING_DEPTH) {
      throw error("JSON nesting exceeds the configured limit");
    }
    require('{');
    skipWhitespace();
    Map<String, Object> object = new LinkedHashMap<>();
    if (consume('}')) {
      return object;
    }
    while (true) {
      skipWhitespace();
      if (peek() != '"') {
        throw error("object field name must be a string");
      }
      String name = readString();
      if (object.containsKey(name)) {
        throw error("duplicate object field: " + name);
      }
      skipWhitespace();
      require(':');
      skipWhitespace();
      object.put(name, readValue(depth + 1));
      skipWhitespace();
      if (consume('}')) {
        return object;
      }
      require(',');
    }
  }

  private Object readValue(int depth) {
    char next = peek();
    if (next == '{') {
      return readObject(depth);
    }
    if (next == '"') {
      return readString();
    }
    if (next == '-' || (next >= '0' && next <= '9')) {
      return readInteger();
    }
    throw error("coordination JSON supports only objects, strings, and integers");
  }

  private Long readInteger() {
    int begin = cursor;
    consume('-');
    if (cursor >= input.length()) {
      throw error("incomplete JSON integer");
    }
    if (input.charAt(cursor) == '0') {
      ++cursor;
      if (cursor < input.length() && Character.isDigit(input.charAt(cursor))) {
        throw error("JSON integer cannot contain a leading zero");
      }
    } else {
      if (input.charAt(cursor) < '1' || input.charAt(cursor) > '9') {
        throw error("invalid JSON integer");
      }
      while (cursor < input.length() && Character.isDigit(input.charAt(cursor))) {
        ++cursor;
      }
    }
    String value = input.substring(begin, cursor);
    if (value.equals("-0")) {
      throw error("JSON integer must use canonical zero");
    }
    try {
      return Long.parseLong(value);
    } catch (NumberFormatException failure) {
      throw error("JSON integer exceeds signed 64-bit range", failure);
    }
  }

  private String readString() {
    require('"');
    StringBuilder value = new StringBuilder();
    while (cursor < input.length()) {
      char character = input.charAt(cursor++);
      if (character == '"') {
        return value.toString();
      }
      if (character == '\\') {
        appendEscape(value);
      } else {
        if (character < 0x20 || Character.isSurrogate(character)) {
          throw error("JSON string contains an invalid unescaped character");
        }
        value.append(character);
      }
    }
    throw error("unterminated JSON string");
  }

  private void appendEscape(StringBuilder value) {
    if (cursor >= input.length()) {
      throw error("unterminated JSON escape");
    }
    char escape = input.charAt(cursor++);
    switch (escape) {
      case '"', '\\', '/' -> value.append(escape);
      case 'b' -> value.append('\b');
      case 'f' -> value.append('\f');
      case 'n' -> value.append('\n');
      case 'r' -> value.append('\r');
      case 't' -> value.append('\t');
      case 'u' -> appendUnicodeEscape(value);
      default -> throw error("invalid JSON escape");
    }
  }

  private void appendUnicodeEscape(StringBuilder value) {
    char first = readHexCharacter();
    if (Character.isLowSurrogate(first)) {
      throw error("JSON string contains an unpaired low surrogate");
    }
    if (!Character.isHighSurrogate(first)) {
      value.append(first);
      return;
    }
    if (cursor + 2 > input.length()
        || input.charAt(cursor) != '\\'
        || input.charAt(cursor + 1) != 'u') {
      throw error("JSON string contains an unpaired high surrogate");
    }
    cursor += 2;
    char second = readHexCharacter();
    if (!Character.isLowSurrogate(second)) {
      throw error("JSON string contains an invalid surrogate pair");
    }
    value.append(first).append(second);
  }

  private char readHexCharacter() {
    if (cursor + 4 > input.length()) {
      throw error("incomplete JSON unicode escape");
    }
    int result = 0;
    for (int index = 0; index < 4; ++index) {
      char character = input.charAt(cursor++);
      int digit = Character.digit(character, 16);
      if (digit < 0) {
        throw error("invalid JSON unicode escape");
      }
      result = result * 16 + digit;
    }
    return (char) result;
  }

  private void skipWhitespace() {
    while (cursor < input.length()) {
      char character = input.charAt(cursor);
      if (character != ' ' && character != '\n' && character != '\r' && character != '\t') {
        return;
      }
      ++cursor;
    }
  }

  private char peek() {
    if (cursor >= input.length()) {
      throw error("unexpected end of JSON");
    }
    return input.charAt(cursor);
  }

  private boolean consume(char expected) {
    if (cursor < input.length() && input.charAt(cursor) == expected) {
      ++cursor;
      return true;
    }
    return false;
  }

  private void require(char expected) {
    if (!consume(expected)) {
      throw error("expected '" + expected + "'");
    }
  }

  private IllegalArgumentException error(String message) {
    return new IllegalArgumentException(message + " at JSON offset " + cursor);
  }

  private IllegalArgumentException error(String message, Throwable cause) {
    return new IllegalArgumentException(message + " at JSON offset " + cursor, cause);
  }
}
