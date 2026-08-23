package com.agoessling.swingcapture;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.Map;

/** Raw-request contract coverage for the protocol code used by the real Android node server. */
public final class NodeHttpProtocolTest {
  private static final String CONTROL_TOKEN = "0123456789abcdefghijklmnopqrstuv";

  private NodeHttpProtocolTest() {}

  public static void main(String[] arguments) throws Exception {
    authorizationPrecedesHistoricalControlMutations();
    immutableMediaRoutesCannotDriftFromBrowserPaths();
    scopedMediaCapabilitiesCannotEscapeOrSurviveRotation();
    unauthorizedResponseIsActionableAndCorsVisible();
    preflightExposesAuthorizationAndRangeHeaders();
    mediaResponsesCoverWholeHeadRangeAndMissingPublication();
  }

  private static void authorizationPrecedesHistoricalControlMutations() throws Exception {
    NodeHttpRequest arm =
        request(
            "POST /api/v1/capture/arm HTTP/1.1\r\n"
                + "Host: phone\r\n"
                + "Content-Type: application/json\r\n"
                + "Content-Length: 55\r\n\r\n"
                + "{\"armed\":true,\"shared_session_id\":\"shared-session-001\"}");
    check(NodeHttpProtocol.requiresControlCredential(arm.method, arm.path), "arm requires bearer");
    check(
        !NodeHttpProtocol.hasValidControlCredential(
            arm.headers.get("authorization"), CONTROL_TOKEN),
        "missing arm credential rejected");
    NodeHttpProtocol.ControlRoute armRoute =
        NodeHttpProtocol.controlRoute(arm.method, arm.path);
    check(armRoute != null, "arm route classified");
    check(
        armRoute.operation() == NodeHttpProtocol.ControlOperation.CAPTURE_ARM,
        "arm operation preserved");
    check(armRoute.successStatus() == 202, "arm remains asynchronous");

    NodeHttpRequest missed =
        request(
            "POST /api/v1/capture/missed-shot HTTP/1.1\r\n"
                + "Host: phone\r\n"
                + "Authorization: Bearer "
                + CONTROL_TOKEN
                + "\r\nContent-Length: 0\r\n\r\n");
    check(
        NodeHttpProtocol.hasValidControlCredential(
            missed.headers.get("authorization"), CONTROL_TOKEN),
        "exact missed-shot credential accepted");
    NodeHttpProtocol.ControlRoute missedRoute =
        NodeHttpProtocol.controlRoute(missed.method, missed.path);
    check(missedRoute != null, "missed-shot route classified");
    check(
        missedRoute.operation() == NodeHttpProtocol.ControlOperation.MISSED_SHOT,
        "missed-shot operation preserved");
    check(missedRoute.successStatus() == 202, "missed-shot remains asynchronous");

    check(
        NodeHttpProtocol.requiresControlCredential("GET", NodeHttpProtocol.CAPTURE_STATUS_PATH),
        "live arm status requires the hosted browser credential");
    check(
        NodeHttpProtocol.controlRoute("GET", NodeHttpProtocol.CAPTURE_ARM_PATH) == null,
        "GET cannot mutate arm state");
    check(
        !NodeHttpProtocol.hasValidControlCredential("bearer " + CONTROL_TOKEN, CONTROL_TOKEN),
        "bearer scheme stays exact");
    check(
        NodeHttpProtocol.requiresControlCredential("GET", "/api/v1/setup"),
        "setup identity remains protected");
    for (String sensitiveRead :
        new String[] {
          "/api/v1/capture/status",
          "/api/v1/capture/trigger-report",
          "/api/v1/discovery",
          "/api/v1/field-recording/status",
          "/api/v1/field-recordings",
          "/api/v1/node",
          "/api/v1/pairing/identity",
          "/api/v1/setup/preview",
          "/api/v1/sessions",
          "/api/v1/coordination/shared-001",
          "/api/v1/future-sensitive-route"
        }) {
      check(
          NodeHttpProtocol.requiresControlCredential("GET", sensitiveRead),
          "sensitive API read requires bearer: " + sensitiveRead);
      check(
          NodeHttpProtocol.requiresControlCredential("HEAD", sensitiveRead),
          "sensitive API HEAD requires bearer: " + sensitiveRead);
    }
    check(
        !NodeHttpProtocol.requiresControlCredential(
            "GET", NodeHttpProtocol.PUBLIC_CLOCK_HINT_PATH),
        "read-only clock hint remains public");
    check(
        !NodeHttpProtocol.requiresControlCredential(
            "HEAD", NodeHttpProtocol.PUBLIC_CLOCK_HINT_PATH),
        "read-only clock hint remains public");
    check(
        NodeHttpProtocol.requiresControlCredential(
            "POST", NodeHttpProtocol.PUBLIC_CLOCK_HINT_PATH),
        "public clock policy cannot exempt mutations");
    check(
        NodeHttpProtocol.publicClockHintResponseHeaders().get("Cache-Control")
            .equals("no-store, max-age=0"),
        "public timestamp samples cannot be cached as fresh evidence");
    check(
        !NodeHttpProtocol.requiresControlCredential("GET", "/app.js"),
        "hosted static application remains public");
    check(
        NodeHttpProtocol.requiresControlCredential(
            "GET", "/api/v1/sessions/session-001/diagnostics.zip"),
        "diagnostic archive remains protected");
  }

  private static void immutableMediaRoutesCannotDriftFromBrowserPaths() {
    NodeHttpProtocol.MediaRoute manifest =
        NodeHttpProtocol.mediaRoute("GET", "/api/v1/sessions/session-001/manifest");
    check(manifest != null, "session manifest route");
    check(manifest.kind() == NodeHttpProtocol.MediaKind.SESSION_MANIFEST, "manifest kind");
    check(manifest.collectionDirectory().equals("sessions"), "manifest collection");
    check(manifest.fileName().equals("manifest.json"), "manifest disk name");
    check(manifest.contentType().equals("application/json"), "manifest type");

    NodeHttpProtocol.MediaRoute video =
        NodeHttpProtocol.mediaRoute(
            "HEAD", "/api/v1/sessions/session-001/down_the_line.mp4");
    check(video != null, "session video route");
    check(video.kind() == NodeHttpProtocol.MediaKind.SESSION_VIDEO, "video kind");
    check(video.fileName().equals("down_the_line.mp4"), "role media name retained");
    check(video.contentType().equals("video/mp4"), "video type");
    check(
        NodeHttpProtocol.requiresControlCredential(
            "GET", "/api/v1/sessions/session-001/manifest"),
        "session manifest requires bearer authorization");
    check(
        NodeHttpProtocol.requiresControlCredential("GET", "/api/v1/field-recordings"),
        "field media catalog requires bearer authorization");

    NodeHttpProtocol.MediaRoute fieldAudio =
        NodeHttpProtocol.mediaRoute(
            "GET", "/api/v1/field-recordings/recording-001/audio.wav");
    check(fieldAudio != null, "field audio route");
    check(fieldAudio.kind() == NodeHttpProtocol.MediaKind.FIELD_AUDIO, "field audio kind");
    check(fieldAudio.collectionDirectory().equals("field_recordings"), "field collection");
    check(fieldAudio.contentType().equals("audio/wav"), "field audio type");

    check(
        NodeHttpProtocol.mediaRoute(
                "GET", "/api/v1/sessions/../down_the_line.mp4")
            == null,
        "parent traversal rejected");
    check(
        NodeHttpProtocol.mediaRoute(
                "POST", "/api/v1/sessions/session-001/down_the_line.mp4")
            == null,
        "media route is immutable");
    check(
        NodeHttpProtocol.mediaRoute(
                "GET", "/api/v1/sessions/session-001/diagnostics.zip")
            == null,
        "diagnostics retain their separately authenticated route");
  }

  private static void scopedMediaCapabilitiesCannotEscapeOrSurviveRotation() {
    NodeHttpProtocol.MediaRoute video =
        NodeHttpProtocol.mediaRoute(
            "GET", "/api/v1/sessions/session-001/down_the_line.mp4");
    NodeHttpProtocol.MediaRoute otherVideo =
        NodeHttpProtocol.mediaRoute(
            "GET", "/api/v1/sessions/session-002/down_the_line.mp4");
    NodeHttpProtocol.MediaRoute manifest =
        NodeHttpProtocol.mediaRoute("GET", "/api/v1/sessions/session-001/manifest");
    NodeHttpProtocol.MediaRoute fieldAudio =
        NodeHttpProtocol.mediaRoute(
            "GET", "/api/v1/field-recordings/recording-001/audio.wav");
    check(video != null && otherVideo != null && manifest != null && fieldAudio != null, "routes");

    String sessionQuery = NodeHttpProtocol.mediaAccessQuery(video, CONTROL_TOKEN);
    check(
        sessionQuery.matches("media_access=[A-Za-z0-9_-]{43}"),
        "capability has one URL-safe fixed-width value");
    check(
        NodeHttpProtocol.hasValidMediaCredential(null, sessionQuery, video, CONTROL_TOKEN),
        "native media capability accepted for its immutable session");
    check(
        NodeHttpProtocol.hasValidMediaCredential(
            "Bearer " + CONTROL_TOKEN, "", video, CONTROL_TOKEN),
        "peer clients retain bearer media access");
    check(
        !NodeHttpProtocol.hasValidMediaCredential(null, sessionQuery, otherVideo, CONTROL_TOKEN),
        "capability cannot escape to another session");
    check(
        !NodeHttpProtocol.hasValidMediaCredential(null, sessionQuery + "&x=1", video, CONTROL_TOKEN),
        "capability query rejects appended ambiguity");
    check(
        !NodeHttpProtocol.hasValidMediaCredential(
            null, sessionQuery, video, "abcdefghijklmnopqrstuvwxyz012345"),
        "control rotation invalidates prior capabilities");
    check(
        !NodeHttpProtocol.hasValidMediaCredential(null, sessionQuery, manifest, CONTROL_TOKEN),
        "session manifest never accepts a native media capability");

    String fieldQuery = NodeHttpProtocol.mediaAccessQuery(fieldAudio, CONTROL_TOKEN);
    check(
        NodeHttpProtocol.hasValidMediaCredential(null, fieldQuery, fieldAudio, CONTROL_TOKEN),
        "field recording audio accepts its scoped capability");
    check(!fieldQuery.equals(sessionQuery), "collection and identifier are domain separated");
  }

  private static void unauthorizedResponseIsActionableAndCorsVisible() throws Exception {
    ByteArrayOutputStream output = new ByteArrayOutputStream();
    NodeHttpResponseWriter.writeUnauthorized(output, false);
    Response response = response(output);
    check(response.statusLine().equals("HTTP/1.1 401 Unauthorized"), "401 status");
    check(response.header("WWW-Authenticate").equals("Bearer"), "bearer challenge");
    check(response.header("Access-Control-Allow-Origin").equals("*"), "401 visible to browser");
    check(
        response.bodyText().equals(
            "{\"error\":\"a valid bearer control credential is required\"}\n"),
        "401 diagnostic body");
    check(
        Long.parseLong(response.header("Content-Length")) == response.body().length,
        "401 length exact");

    output.reset();
    NodeHttpResponseWriter.writeMediaUnauthorized(output, false);
    Response media = response(output);
    check(media.statusLine().equals("HTTP/1.1 401 Unauthorized"), "media 401 status");
    check(
        media.bodyText().contains("scoped media capability"),
        "media 401 explains native authorization");

    output.reset();
    NodeHttpResponseWriter.writeUnauthorized(output, true);
    Response head = response(output);
    check(head.body().length == 0, "HEAD 401 omits body");
    check(Long.parseLong(head.header("Content-Length")) > 0, "HEAD retains entity length");
  }

  private static void preflightExposesAuthorizationAndRangeHeaders() throws Exception {
    ByteArrayOutputStream output = new ByteArrayOutputStream();
    NodeHttpResponseWriter.writeHeaders(
        output, 204, "No Content", "text/plain", 0, Map.of());
    Response response = response(output);
    check(response.statusLine().equals("HTTP/1.1 204 No Content"), "preflight status");
    check(
        response.header("Access-Control-Allow-Methods").equals("GET, HEAD, POST, PUT, OPTIONS"),
        "browser methods admitted");
    check(
        response.header("Access-Control-Allow-Headers")
            .equals("Authorization, Range, Content-Type"),
        "credential and byte range headers admitted");
    check(
        response.header("Access-Control-Expose-Headers").contains("Content-Range"),
        "range response visible");
    check(
        response.header("Access-Control-Expose-Headers")
            .contains(MediaAccessAuthorization.RESPONSE_HEADER),
        "scoped media capability response visible");
  }

  private static void mediaResponsesCoverWholeHeadRangeAndMissingPublication() throws Exception {
    Path directory = Files.createTempDirectory("node-http-protocol-test-");
    Path media = directory.resolve("down_the_line.mp4");
    Path largeMedia = directory.resolve("large.mp4");
    byte[] content = new byte[] {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    try {
      Files.write(media, content);

      ByteArrayOutputStream output = new ByteArrayOutputStream();
      NodeHttpResponseWriter.serveFile(
          request(
              "HEAD /api/v1/sessions/session-001/down_the_line.mp4 HTTP/1.1\r\n"
                  + "Host: phone\r\n\r\n"),
          output,
          media.toFile(),
          "video/mp4",
          true,
          Map.of());
      Response head = response(output);
      check(head.statusLine().equals("HTTP/1.1 200 OK"), "published media HEAD status");
      check(head.header("Content-Length").equals("10"), "published media byte count");
      check(head.header("Accept-Ranges").equals("bytes"), "published media seek support");
      check(head.body().length == 0, "HEAD has no media body");

      output.reset();
      NodeHttpResponseWriter.serveFile(
          request(
              "GET /api/v1/sessions/session-001/down_the_line.mp4 HTTP/1.1\r\n"
                  + "Host: phone\r\nRange: bytes=2-5\r\n\r\n"),
          output,
          media.toFile(),
          "video/mp4",
          false,
          Map.of());
      Response partial = response(output);
      check(partial.statusLine().equals("HTTP/1.1 206 Partial Content"), "range status");
      check(partial.header("Content-Range").equals("bytes 2-5/10"), "range bounds");
      check(Arrays.equals(partial.body(), new byte[] {2, 3, 4, 5}), "range bytes exact");

      byte[] largeContent =
          new byte[(int) NodeHttpResponseWriter.MAXIMUM_PARTIAL_RESPONSE_BYTES + 17];
      for (int index = 0; index < largeContent.length; ++index) {
        largeContent[index] = (byte) index;
      }
      Files.write(largeMedia, largeContent);
      output.reset();
      NodeHttpResponseWriter.serveFile(
          request(
              "GET /api/v1/field-recordings/field-1/video.mp4 HTTP/1.1\r\n"
                  + "Host: phone\r\nRange: bytes=0-\r\n\r\n"),
          output,
          largeMedia.toFile(),
          "video/mp4",
          false,
          Map.of());
      Response bounded = response(output);
      long maximum = NodeHttpResponseWriter.MAXIMUM_PARTIAL_RESPONSE_BYTES;
      check(
          bounded.header("Content-Range")
              .equals("bytes 0-" + (maximum - 1) + "/" + largeContent.length),
          "open-ended range response is bounded");
      check(bounded.body().length == maximum, "bounded range body length exact");

      output.reset();
      NodeHttpResponseWriter.serveFile(
          request(
              "GET /api/v1/sessions/session-001/down_the_line.mp4 HTTP/1.1\r\n"
                  + "Host: phone\r\nRange: bytes=20-30\r\n\r\n"),
          output,
          media.toFile(),
          "video/mp4",
          false,
          Map.of());
      Response invalid = response(output);
      check(
          invalid.statusLine().equals("HTTP/1.1 416 Range Not Satisfiable"),
          "invalid range status");
      check(invalid.header("Content-Range").equals("bytes */10"), "invalid range size");

      output.reset();
      NodeHttpResponseWriter.serveFile(
          request(
              "GET /api/v1/sessions/session-001/face_on.mp4 HTTP/1.1\r\n"
                  + "Host: phone\r\n\r\n"),
          output,
          directory.resolve("face_on.mp4").toFile(),
          "video/mp4",
          false,
          Map.of());
      Response missing = response(output);
      check(missing.statusLine().equals("HTTP/1.1 404 Not Found"), "missing clip status");
      check(missing.bodyText().equals("{\"error\":\"artifact not found\"}\n"), "missing clip body");
      check(
          missing.header("Access-Control-Allow-Origin").equals("*"),
          "cross-origin browser can diagnose missing clip");
    } finally {
      Files.deleteIfExists(largeMedia);
      Files.deleteIfExists(media);
      Files.deleteIfExists(directory);
    }
  }

  private static NodeHttpRequest request(String wire) throws Exception {
    return NodeHttpRequestParser.read(
        new ByteArrayInputStream(wire.getBytes(StandardCharsets.ISO_8859_1)),
        16 * 1024,
        64 * 1024);
  }

  private static Response response(ByteArrayOutputStream output) {
    byte[] wire = output.toByteArray();
    byte[] separator = "\r\n\r\n".getBytes(StandardCharsets.ISO_8859_1);
    int boundary = indexOf(wire, separator);
    check(boundary >= 0, "response header terminator");
    String headerText = new String(wire, 0, boundary, StandardCharsets.ISO_8859_1);
    String[] lines = headerText.split("\r\n");
    java.util.HashMap<String, String> headers = new java.util.HashMap<>();
    for (int index = 1; index < lines.length; ++index) {
      int colon = lines[index].indexOf(':');
      check(colon > 0, "response header shape");
      headers.put(lines[index].substring(0, colon), lines[index].substring(colon + 1).trim());
    }
    int bodyStart = boundary + separator.length;
    return new Response(
        lines[0], Map.copyOf(headers), Arrays.copyOfRange(wire, bodyStart, wire.length));
  }

  private static int indexOf(byte[] haystack, byte[] needle) {
    for (int start = 0; start <= haystack.length - needle.length; ++start) {
      boolean matches = true;
      for (int index = 0; index < needle.length; ++index) {
        if (haystack[start + index] != needle[index]) {
          matches = false;
          break;
        }
      }
      if (matches) {
        return start;
      }
    }
    return -1;
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }

  private record Response(String statusLine, Map<String, String> headers, byte[] body) {
    private Response {
      body = body.clone();
    }

    String header(String name) {
      String value = headers.get(name);
      if (value == null) {
        throw new AssertionError("missing response header " + name);
      }
      return value;
    }

    @Override
    public byte[] body() {
      return body.clone();
    }

    String bodyText() {
      return new String(body, StandardCharsets.UTF_8);
    }
  }
}
