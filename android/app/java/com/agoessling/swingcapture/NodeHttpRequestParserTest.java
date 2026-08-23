package com.agoessling.swingcapture;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

public final class NodeHttpRequestParserTest {
  private static final int HEADER_LIMIT = 1_024;
  private static final int BODY_LIMIT = 32;

  private NodeHttpRequestParserTest() {}

  public static void main(String[] arguments) throws Exception {
    acceptsOnlyStrictHttp1RequestLines();
    separatesQueryWithoutDecodingTheTarget();
    normalizesHeaderNamesCaseInsensitively();
    rejectsDuplicateSecuritySensitiveHeaders();
    rejectsUnsupportedTransferEncoding();
    readsExactlyTheDeclaredBody();
    treatsMissingContentLengthAsAnEmptyBody();
    rejectsInvalidNegativeAndOversizedContentLength();
    rejectsTruncatedBodies();
    enforcesHeaderByteCapIncludingTerminator();
  }

  private static void acceptsOnlyStrictHttp1RequestLines() throws Exception {
    assertEquals("GET", parse("GET / HTTP/1.0\r\n\r\n").method, "HTTP/1.0 method");
    assertEquals("PATCH", parse("PATCH /node HTTP/1.1\r\n\r\n").method, "HTTP/1.1 method");

    for (String line :
        new String[] {
          "GET  / HTTP/1.1",
          "GET /  HTTP/1.1",
          " GET / HTTP/1.1",
          "GET / HTTP/1.1 ",
          "GET / HTTP/1.2",
          "GET / HTTP/2",
          "GET https://phone.example/ HTTP/1.1",
          "GET /review#fragment HTTP/1.1",
          "GE(T / HTTP/1.1",
        }) {
      assertRejected(line + "\r\n\r\n", HEADER_LIMIT, BODY_LIMIT, "request line " + line);
    }
  }

  private static void separatesQueryWithoutDecodingTheTarget() throws Exception {
    NodeHttpRequest request =
        parse("GET /api/v1/setup/preview?generation=7&cache=false HTTP/1.1\r\n\r\n");
    assertEquals("/api/v1/setup/preview", request.path, "query-stripped path");
    assertEquals("generation=7&cache=false", request.query, "raw query retained");

    NodeHttpRequest encoded = parse("GET /sessions%2Flatest? HTTP/1.1\r\n\r\n");
    assertEquals("/sessions%2Flatest", encoded.path, "encoded path remains encoded");
    assertEquals("", encoded.query, "empty query retained as empty");

    NodeHttpRequest noQuery = parse("GET /sessions/latest HTTP/1.1\r\n\r\n");
    assertEquals("", noQuery.query, "absent query retained as empty");
  }

  private static void normalizesHeaderNamesCaseInsensitively() throws Exception {
    NodeHttpRequest request =
        parse(
            "GET /api/v1/setup HTTP/1.1\r\n"
                + "AuThOrIzAtIoN:  Bearer demo-token \t\r\n"
                + "X-Node-Label: first\r\n"
                + "x-node-label: second\r\n\r\n");
    assertEquals("Bearer demo-token", request.headers.get("authorization"), "authorization");
    assertEquals("second", request.headers.get("x-node-label"), "non-security duplicate");
    check(!request.headers.containsKey("AuThOrIzAtIoN"), "header keys are lowercase");
  }

  private static void rejectsDuplicateSecuritySensitiveHeaders() {
    for (String header :
        new String[] {
          "Authorization: Bearer one\r\nauthorization: Bearer two",
          "Proxy-Authorization: one\r\nproxy-authorization: two",
          "Host: phone-one\r\nhost: phone-two",
          "Content-Length: 0\r\ncontent-length: 0",
          "Transfer-Encoding: chunked\r\ntransfer-encoding: identity",
        }) {
      assertRejected(
          "GET / HTTP/1.1\r\n" + header + "\r\n\r\n",
          HEADER_LIMIT,
          BODY_LIMIT,
          "duplicate singleton " + header);
    }
  }

  private static void rejectsUnsupportedTransferEncoding() {
    for (String value : new String[] {"chunked", "identity", "gzip, chunked", ""}) {
      assertRejected(
          "POST /api/v1/capture/arm HTTP/1.1\r\nTransfer-Encoding: "
              + value
              + "\r\n\r\n",
          HEADER_LIMIT,
          BODY_LIMIT,
          "transfer encoding " + value);
    }
  }

  private static void readsExactlyTheDeclaredBody() throws Exception {
    NodeHttpRequest request = parse(requestWithContentLength("004", "bodytail"));
    assertEquals("body", request.bodyText(), "declared request body");
  }

  private static void treatsMissingContentLengthAsAnEmptyBody() throws Exception {
    byte[] bytes =
        "POST /api/v1/capture/missed-shot HTTP/1.1\r\n\r\nignored"
            .getBytes(StandardCharsets.ISO_8859_1);
    ByteArrayInputStream input = new ByteArrayInputStream(bytes);
    NodeHttpRequest request = NodeHttpRequestParser.read(input, HEADER_LIMIT, BODY_LIMIT);
    check(request.body.length == 0, "missing Content-Length means an empty body");
    check(input.available() == "ignored".length(), "unframed bytes are not consumed");
  }

  private static void rejectsInvalidNegativeAndOversizedContentLength() {
    for (String value :
        new String[] {
          "",
          "-1",
          "+1",
          "1.0",
          "1, 1",
          "9223372036854775808",
        }) {
      assertRejected(
          requestWithContentLength(value, ""),
          HEADER_LIMIT,
          BODY_LIMIT,
          "invalid Content-Length " + value);
    }
    assertRejected(
        requestWithContentLength(Integer.toString(BODY_LIMIT + 1), ""),
        HEADER_LIMIT,
        BODY_LIMIT,
        "oversized Content-Length");
  }

  private static void rejectsTruncatedBodies() {
    assertRejected(
        requestWithContentLength("4", "abc"), HEADER_LIMIT, BODY_LIMIT, "truncated body");
  }

  private static void enforcesHeaderByteCapIncludingTerminator() throws Exception {
    String requestText = "GET / HTTP/1.1\r\nX-Padding: 123456789\r\n\r\n";
    int exactBytes = requestText.getBytes(StandardCharsets.ISO_8859_1).length;
    NodeHttpRequest exact = parse(requestText, exactBytes, BODY_LIMIT);
    assertEquals("123456789", exact.headers.get("x-padding"), "exact-cap header");
    assertRejected(requestText, exactBytes - 1, BODY_LIMIT, "header one byte over cap");

    assertRejected(
        "GET / HTTP/1.1\r\nX-Padding: unterminated",
        24,
        BODY_LIMIT,
        "unterminated capped header");
  }

  private static NodeHttpRequest parse(String request) throws Exception {
    return parse(request, HEADER_LIMIT, BODY_LIMIT);
  }

  private static NodeHttpRequest parse(String request, int headerLimit, int bodyLimit)
      throws Exception {
    return NodeHttpRequestParser.read(
        new ByteArrayInputStream(request.getBytes(StandardCharsets.ISO_8859_1)),
        headerLimit,
        bodyLimit);
  }

  private static String requestWithContentLength(String contentLength, String body) {
    return "POST /api/v1/capture/arm HTTP/1.1\r\nContent-Length: "
        + contentLength
        + "\r\n\r\n"
        + body;
  }

  private static void assertRejected(
      String request, int headerLimit, int bodyLimit, String label) {
    try {
      parse(request, headerLimit, bodyLimit);
      throw new AssertionError("Expected rejection for " + label);
    } catch (IOException expected) {
      // Expected.
    } catch (Exception unexpected) {
      throw new AssertionError("Unexpected exception for " + label, unexpected);
    }
  }

  private static void assertEquals(String expected, String actual, String label) {
    if (!expected.equals(actual)) {
      throw new AssertionError(label + ": expected " + expected + ", got " + actual);
    }
  }

  private static void check(boolean condition, String label) {
    if (!condition) {
      throw new AssertionError(label);
    }
  }
}
