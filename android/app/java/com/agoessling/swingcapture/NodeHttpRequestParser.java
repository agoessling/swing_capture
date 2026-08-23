package com.agoessling.swingcapture;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/** Strict, bounded HTTP/1 request parser shared by production and host-side contract tests. */
final class NodeHttpRequestParser {
  private static final Set<String> SINGLETON_SECURITY_HEADERS =
      Set.of(
          "authorization",
          "proxy-authorization",
          "host",
          "content-length",
          "transfer-encoding");

  private NodeHttpRequestParser() {}

  static NodeHttpRequest read(
      InputStream input, int maximumHeaderBytes, int maximumBodyBytes) throws IOException {
    if (input == null) {
      throw new NullPointerException("input");
    }
    if (maximumHeaderBytes <= 0 || maximumBodyBytes < 0) {
      throw new IllegalArgumentException("HTTP request limits are invalid");
    }

    byte[] headerBytes = readHeaders(input, maximumHeaderBytes);
    String headerText = new String(headerBytes, StandardCharsets.ISO_8859_1);
    String[] lines = headerText.substring(0, headerText.length() - 4).split("\\r\\n", -1);
    if (lines.length == 0) {
      throw new IOException("Missing HTTP request line");
    }
    RequestLine requestLine = parseRequestLine(lines[0]);
    Map<String, String> headers = parseHeaders(lines);
    if (headers.containsKey("transfer-encoding")) {
      throw new IOException("Transfer-Encoding is not supported");
    }

    int contentLength = contentLength(headers.get("content-length"), maximumBodyBytes);
    byte[] body = input.readNBytes(contentLength);
    if (body.length != contentLength) {
      throw new IOException("Client disconnected before sending the request body");
    }
    return new NodeHttpRequest(
        requestLine.method(), requestLine.path(), requestLine.query(), headers, body);
  }

  private static byte[] readHeaders(InputStream input, int maximumHeaderBytes) throws IOException {
    ByteArrayOutputStream bytes = new ByteArrayOutputStream();
    int delimiterState = 0;
    while (bytes.size() < maximumHeaderBytes) {
      int value = input.read();
      if (value < 0) {
        throw new IOException("Client disconnected before sending request headers");
      }
      bytes.write(value);
      if ((delimiterState == 0 || delimiterState == 2) && value == '\r') {
        ++delimiterState;
      } else if ((delimiterState == 1 || delimiterState == 3) && value == '\n') {
        ++delimiterState;
        if (delimiterState == 4) {
          return bytes.toByteArray();
        }
      } else {
        delimiterState = 0;
      }
    }
    throw new IOException("Request headers exceed the configured limit");
  }

  private static RequestLine parseRequestLine(String line) throws IOException {
    int firstSpace = line.indexOf(' ');
    int secondSpace = firstSpace < 0 ? -1 : line.indexOf(' ', firstSpace + 1);
    if (firstSpace <= 0
        || secondSpace <= firstSpace + 1
        || secondSpace == line.length() - 1
        || line.indexOf(' ', secondSpace + 1) >= 0) {
      throw new IOException("Malformed HTTP request line");
    }
    String method = line.substring(0, firstSpace);
    String target = line.substring(firstSpace + 1, secondSpace);
    String version = line.substring(secondSpace + 1);
    if (!isToken(method)
        || (!version.equals("HTTP/1.0") && !version.equals("HTTP/1.1"))
        || !target.startsWith("/")
        || target.indexOf('#') >= 0
        || containsControlCharacter(target)) {
      throw new IOException("Malformed HTTP request line");
    }
    int query = target.indexOf('?');
    String path = query < 0 ? target : target.substring(0, query);
    String queryString = query < 0 ? "" : target.substring(query + 1);
    return new RequestLine(method, path, queryString);
  }

  private static Map<String, String> parseHeaders(String[] lines) throws IOException {
    Map<String, String> headers = new HashMap<>();
    for (int index = 1; index < lines.length; ++index) {
      String line = lines[index];
      if (line.isEmpty() || line.charAt(0) == ' ' || line.charAt(0) == '\t') {
        throw new IOException("Malformed HTTP request header");
      }
      int separator = line.indexOf(':');
      if (separator <= 0) {
        throw new IOException("Malformed HTTP request header");
      }
      String originalName = line.substring(0, separator);
      if (!isToken(originalName)) {
        throw new IOException("Malformed HTTP request header name");
      }
      String name = originalName.toLowerCase(Locale.ROOT);
      String value = trimOptionalWhitespace(line.substring(separator + 1));
      if (containsInvalidHeaderValueCharacter(value)) {
        throw new IOException("Malformed HTTP request header value");
      }
      if (headers.containsKey(name) && SINGLETON_SECURITY_HEADERS.contains(name)) {
        throw new IOException("Duplicate security-sensitive HTTP request header: " + name);
      }
      // Preserve the former last-value behavior for non-security headers. Routes that assign
      // semantics to another singleton header can promote it to the guarded set above.
      headers.put(name, value);
    }
    return headers;
  }

  private static int contentLength(String value, int maximumBodyBytes) throws IOException {
    if (value == null) {
      return 0;
    }
    if (value.isEmpty()) {
      throw new IOException("Invalid Content-Length");
    }
    long parsed = 0;
    for (int index = 0; index < value.length(); ++index) {
      char digit = value.charAt(index);
      if (digit < '0' || digit > '9') {
        throw new IOException("Invalid Content-Length");
      }
      if (parsed > (Long.MAX_VALUE - (digit - '0')) / 10) {
        throw new IOException("Invalid Content-Length");
      }
      parsed = parsed * 10 + (digit - '0');
    }
    if (parsed > maximumBodyBytes) {
      throw new IOException("Request body exceeds the configured limit");
    }
    return (int) parsed;
  }

  private static boolean isToken(String value) {
    if (value.isEmpty()) {
      return false;
    }
    for (int index = 0; index < value.length(); ++index) {
      char character = value.charAt(index);
      boolean tokenCharacter =
          (character >= '0' && character <= '9')
              || (character >= 'A' && character <= 'Z')
              || (character >= 'a' && character <= 'z')
              || "!#$%&'*+-.^_`|~".indexOf(character) >= 0;
      if (!tokenCharacter) {
        return false;
      }
    }
    return true;
  }

  private static String trimOptionalWhitespace(String value) {
    int start = 0;
    int end = value.length();
    while (start < end && (value.charAt(start) == ' ' || value.charAt(start) == '\t')) {
      ++start;
    }
    while (end > start && (value.charAt(end - 1) == ' ' || value.charAt(end - 1) == '\t')) {
      --end;
    }
    return value.substring(start, end);
  }

  private static boolean containsControlCharacter(String value) {
    for (int index = 0; index < value.length(); ++index) {
      char character = value.charAt(index);
      if (character <= 0x1f || character == 0x7f) {
        return true;
      }
    }
    return false;
  }

  private static boolean containsInvalidHeaderValueCharacter(String value) {
    for (int index = 0; index < value.length(); ++index) {
      char character = value.charAt(index);
      if ((character < 0x20 && character != '\t') || character == 0x7f) {
        return true;
      }
    }
    return false;
  }

  private record RequestLine(String method, String path, String query) {}
}
