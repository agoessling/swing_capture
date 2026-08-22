package com.agoessling.swingcapture;

import com.agoessling.swingcapture.node.BearerAuthorization;
import com.agoessling.swingcapture.node.NodeCoordinationState;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.math.BigDecimal;
import java.net.HttpURLConnection;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.Objects;

/** Bounded authenticated leader-to-peer pose arm request. */
public final class PosePeerArmClient {
  private static final int TIMEOUT_MILLIS = 1_500;
  private static final int MAXIMUM_RESPONSE_BYTES = 16 * 1024;

  public record Candidate(
      String sharedSessionId,
      String leaderNodeId,
      long candidateElapsedRealtimeNanos,
      double personConfidence,
      double addressConfidence) {
    public Candidate {
      NodeCoordinationState.validateSharedSessionId(sharedSessionId);
      requireIdentifier(leaderNodeId, "leaderNodeId");
      if (candidateElapsedRealtimeNanos <= 0) {
        throw new IllegalArgumentException("candidate timestamp must be positive");
      }
      requireUnitInterval(personConfidence, "personConfidence");
      requireUnitInterval(addressConfidence, "addressConfidence");
    }

    public byte[] requestBody() {
      String json =
          "{\"schema_version\":1,\"shared_session_id\":\""
              + escapeJson(sharedSessionId)
              + "\",\"leader_node_id\":\""
              + escapeJson(leaderNodeId)
              + "\",\"candidate_elapsed_realtime_ns\":\""
              + candidateElapsedRealtimeNanos
              + "\",\"person_confidence\":"
              + canonicalDouble(personConfidence)
              + ",\"address_confidence\":"
              + canonicalDouble(addressConfidence)
              + "}";
      return json.getBytes(StandardCharsets.UTF_8);
    }
  }

  public record Response(int statusCode, String body) {
    public Response {
      if (statusCode < 100 || statusCode > 599) {
        throw new IllegalArgumentException("invalid HTTP status");
      }
      Objects.requireNonNull(body, "body");
    }

    public boolean accepted() {
      return statusCode >= 200 && statusCode < 300;
    }
  }

  @FunctionalInterface
  public interface Transport {
    Response post(URI endpoint, String authorization, byte[] body) throws IOException;
  }

  private final URI endpoint;
  private final String authorization;
  private final Transport transport;

  public PosePeerArmClient(String peerOrigin, String controlToken) {
    this(peerOrigin, controlToken, PosePeerArmClient::postHttp);
  }

  PosePeerArmClient(String peerOrigin, String controlToken, Transport transport) {
    endpoint = poseArmEndpoint(peerOrigin);
    if (!BearerAuthorization.isValidToken(controlToken)) {
      throw new IllegalArgumentException("peer control token is invalid");
    }
    authorization = "Bearer " + controlToken;
    this.transport = Objects.requireNonNull(transport, "transport");
  }

  public Response arm(Candidate candidate) throws IOException {
    Objects.requireNonNull(candidate, "candidate");
    return transport.post(endpoint, authorization, candidate.requestBody());
  }

  URI endpoint() {
    return endpoint;
  }

  private static URI poseArmEndpoint(String peerOrigin) {
    Objects.requireNonNull(peerOrigin, "peerOrigin");
    URI origin;
    try {
      origin = URI.create(peerOrigin);
    } catch (IllegalArgumentException invalid) {
      throw new IllegalArgumentException("peer origin is not a valid URI", invalid);
    }
    if (!"http".equals(origin.getScheme())
        || origin.getHost() == null
        || origin.getUserInfo() != null
        || origin.getQuery() != null
        || origin.getFragment() != null
        || !(origin.getPath().isEmpty() || origin.getPath().equals("/"))
        || origin.getPort() <= 0
        || origin.getPort() > 65_535) {
      throw new IllegalArgumentException("peer origin must be http://host:port with no path");
    }
    return origin.resolve("/api/v1/capture/pose-arm");
  }

  private static Response postHttp(URI endpoint, String authorization, byte[] body)
      throws IOException {
    HttpURLConnection connection = (HttpURLConnection) endpoint.toURL().openConnection();
    connection.setConnectTimeout(TIMEOUT_MILLIS);
    connection.setReadTimeout(TIMEOUT_MILLIS);
    connection.setInstanceFollowRedirects(false);
    connection.setRequestMethod("POST");
    connection.setRequestProperty("Authorization", authorization);
    connection.setRequestProperty("Content-Type", "application/json; charset=utf-8");
    connection.setFixedLengthStreamingMode(body.length);
    connection.setDoOutput(true);
    try {
      try (OutputStream output = connection.getOutputStream()) {
        output.write(body);
      }
      int status = connection.getResponseCode();
      InputStream responseStream =
          status >= 400 ? connection.getErrorStream() : connection.getInputStream();
      String responseBody = "";
      if (responseStream != null) {
        try (InputStream input = responseStream) {
          byte[] contents = input.readNBytes(MAXIMUM_RESPONSE_BYTES + 1);
          if (contents.length > MAXIMUM_RESPONSE_BYTES || input.read() >= 0) {
            throw new IOException("peer response exceeds configured bound");
          }
          responseBody = new String(contents, StandardCharsets.UTF_8);
        }
      }
      return new Response(status, responseBody);
    } finally {
      connection.disconnect();
    }
  }

  private static void requireIdentifier(String value, String name) {
    if (value == null || value.isBlank() || value.length() > 128) {
      throw new IllegalArgumentException(name + " must contain 1 to 128 characters");
    }
    for (int index = 0; index < value.length(); ++index) {
      char character = value.charAt(index);
      if (character < 0x20 || character == '"' || character == '\\') {
        throw new IllegalArgumentException(name + " contains a prohibited character");
      }
    }
  }

  private static void requireUnitInterval(double value, String name) {
    if (!Double.isFinite(value) || value < 0.0 || value > 1.0) {
      throw new IllegalArgumentException(name + " must be finite and in [0, 1]");
    }
  }

  private static String canonicalDouble(double value) {
    return BigDecimal.valueOf(value).stripTrailingZeros().toPlainString();
  }

  private static String escapeJson(String value) {
    return value.replace("\\", "\\\\").replace("\"", "\\\"");
  }
}
