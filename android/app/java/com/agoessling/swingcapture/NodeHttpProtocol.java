package com.agoessling.swingcapture;

import com.agoessling.swingcapture.node.BearerAuthorization;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/** Android-free routing and authorization contract shared by the live node server and tests. */
final class NodeHttpProtocol {
  static final String CAPTURE_STATUS_PATH = "/api/v1/capture/status";
  static final String CAPTURE_ARM_PATH = "/api/v1/capture/arm";
  static final String MISSED_SHOT_PATH = "/api/v1/capture/missed-shot";
  static final String PUBLIC_CLOCK_HINT_PATH = "/api/v1/clock";
  static final String NETWORK_HEALTH_REVERSE_PATH = "/api/v1/network-health/reverse-probe";

  enum ControlOperation {
    CAPTURE_ARM,
    MISSED_SHOT
  }

  record ControlRoute(ControlOperation operation, int successStatus) {
    ControlRoute {
      java.util.Objects.requireNonNull(operation, "operation");
      if (successStatus != 202) {
        throw new IllegalArgumentException("node capture mutations must be asynchronous");
      }
    }
  }

  enum MediaKind {
    SESSION_MANIFEST,
    SESSION_VIDEO,
    FIELD_MANIFEST,
    FIELD_VIDEO,
    FIELD_AUDIO
  }

  record MediaRoute(
      MediaKind kind,
      String collectionDirectory,
      String identifier,
      String fileName,
      String contentType) {
    MediaRoute {
      java.util.Objects.requireNonNull(kind, "kind");
      if (!safeSegment(collectionDirectory)
          || !safeSegment(identifier)
          || !safeSegment(fileName)
          || contentType == null
          || contentType.isBlank()) {
        throw new IllegalArgumentException("media route is invalid");
      }
    }
  }

  // The clock response contains only an untrusted time hint needed before a peer clock mapping is
  // established. Every identity, operational, capture, session, diagnostic, setup, and pairing
  // read remains private, and new API reads fail closed unless deliberately classified here.
  private static final Set<String> PUBLIC_UNTRUSTED_HINT_READ_PATHS =
      Set.of(PUBLIC_CLOCK_HINT_PATH);
  private static final String API_PATH_PREFIX = "/api/v1/";

  private NodeHttpProtocol() {}

  /** Returns whether the live server must reject the request before any mutation or data access. */
  static boolean requiresControlCredential(String method, String path) {
    String normalizedMethod = method.toUpperCase(Locale.ROOT);
    if (normalizedMethod.equals("POST")) {
      return true;
    }
    if (normalizedMethod.equals("PUT")) {
      return path.equals("/api/v1/setup");
    }
    if (!normalizedMethod.equals("GET") && !normalizedMethod.equals("HEAD")) {
      return false;
    }
    MediaRoute mediaRoute = mediaRoute(normalizedMethod, path);
    if (mediaRoute != null) {
      // Native <video>/<audio> requests cannot attach Authorization. Immutable bytes therefore
      // pass through the separate collection-scoped capability check. The manifest grants that
      // capability and consequently always requires the full Bearer credential.
      return mediaRoute.kind() == MediaKind.SESSION_MANIFEST;
    }
    return path.startsWith(API_PATH_PREFIX) && !PUBLIC_UNTRUSTED_HINT_READ_PATHS.contains(path);
  }

  static boolean hasValidControlCredential(
      String authorizationHeader, String expectedControlToken) {
    return BearerAuthorization.accepts(authorizationHeader, expectedControlToken);
  }

  /** Prevents a proxy or client cache from replaying a timestamp sample as if it were fresh. */
  static Map<String, String> publicClockHintResponseHeaders() {
    return Map.of("Cache-Control", "no-store, max-age=0");
  }

  /** Authorizes immutable media through Bearer control or a collection-scoped native capability. */
  static boolean hasValidMediaCredential(
      String authorizationHeader,
      String rawQuery,
      MediaRoute route,
      String expectedControlToken) {
    java.util.Objects.requireNonNull(route, "route");
    if (hasValidControlCredential(authorizationHeader, expectedControlToken)) {
      return true;
    }
    if (route.kind() == MediaKind.SESSION_MANIFEST) {
      return false;
    }
    return MediaAccessAuthorization.accepts(
        rawQuery, route.collectionDirectory(), route.identifier(), expectedControlToken);
  }

  static String mediaAccessQuery(MediaRoute route, String expectedControlToken) {
    java.util.Objects.requireNonNull(route, "route");
    return MediaAccessAuthorization.query(
        route.collectionDirectory(), route.identifier(), expectedControlToken);
  }

  /** Returns capture mutations whose status and ownership semantics are consumed by the browser. */
  static ControlRoute controlRoute(String method, String path) {
    if (!method.equals("POST")) {
      return null;
    }
    if (path.equals(CAPTURE_ARM_PATH)) {
      return new ControlRoute(ControlOperation.CAPTURE_ARM, 202);
    }
    if (path.equals(MISSED_SHOT_PATH)) {
      return new ControlRoute(ControlOperation.MISSED_SHOT, 202);
    }
    return null;
  }

  /** Resolves only immutable review artifacts; diagnostic ZIPs have a separate authenticated path. */
  static MediaRoute mediaRoute(String method, String path) {
    if (!method.equals("GET") && !method.equals("HEAD")) {
      return null;
    }
    String[] segments = path.split("/");
    if (segments.length != 6
        || !segments[1].equals("api")
        || !segments[2].equals("v1")
        || !safeSegment(segments[4])) {
      return null;
    }
    String identifier = segments[4];
    String artifact = segments[5];
    if (segments[3].equals("sessions")) {
      if (artifact.equals("manifest")) {
        return new MediaRoute(
            MediaKind.SESSION_MANIFEST,
            "sessions",
            identifier,
            "manifest.json",
            "application/json");
      }
      if (safeSegment(artifact) && artifact.endsWith(".mp4")) {
        return new MediaRoute(
            MediaKind.SESSION_VIDEO, "sessions", identifier, artifact, "video/mp4");
      }
      return null;
    }
    if (!segments[3].equals("field-recordings")) {
      return null;
    }
    return switch (artifact) {
      case "manifest" ->
          new MediaRoute(
              MediaKind.FIELD_MANIFEST,
              "field_recordings",
              identifier,
              "manifest.json",
              "application/json");
      case "video.mp4" ->
          new MediaRoute(
              MediaKind.FIELD_VIDEO,
              "field_recordings",
              identifier,
              artifact,
              "video/mp4");
      case "audio.wav" ->
          new MediaRoute(
              MediaKind.FIELD_AUDIO,
              "field_recordings",
              identifier,
              artifact,
              "audio/wav");
      default -> null;
    };
  }

  static boolean safeSegment(String value) {
    return value != null
        && !value.isEmpty()
        && !value.equals(".")
        && !value.equals("..")
        && !value.endsWith(".tmp")
        && value.matches("[A-Za-z0-9._-]+");
  }
}
