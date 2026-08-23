package com.agoessling.swingcapture;

import com.agoessling.swingcapture.core.coordination.PairedCoordinationRecord;
import com.agoessling.swingcapture.node.BearerAuthorization;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.Objects;

/** Bounded authenticated transport used by the leader-owned station lifecycle. */
final class AutonomousPairPeerClient {
  private static final int TIMEOUT_MILLIS = 1_500;
  private static final int MAXIMUM_RESPONSE_BYTES = 64 * 1024;

  record Response(int statusCode, String body) {
    Response {
      if (statusCode < 100 || statusCode > 599) {
        throw new IllegalArgumentException("invalid HTTP status");
      }
      Objects.requireNonNull(body, "body");
    }

    boolean accepted() {
      return statusCode >= 200 && statusCode < 300;
    }
  }

  /** Exact two-route proof that one peer capture is ready for browser review. */
  record PublicationProbe(boolean published, int statusCode, String stage) {
    PublicationProbe {
      if (statusCode < 100 || statusCode > 599) {
        throw new IllegalArgumentException("invalid HTTP status");
      }
      if (!stage.equals("manifest") && !stage.equals("media") && !stage.equals("complete")) {
        throw new IllegalArgumentException("invalid publication stage");
      }
      if (published != (statusCode == 200 && stage.equals("complete"))) {
        throw new IllegalArgumentException("publication result fields disagree");
      }
    }
  }

  @FunctionalInterface
  interface Transport {
    Response request(URI endpoint, String method, String authorization, byte[] body)
        throws IOException;
  }

  private final URI origin;
  private final String authorization;
  private final Transport transport;

  AutonomousPairPeerClient(String peerOrigin, String controlToken) {
    this(peerOrigin, controlToken, AutonomousPairPeerClient::requestHttp);
  }

  AutonomousPairPeerClient(String peerOrigin, String controlToken, Transport transport) {
    origin = validateOrigin(peerOrigin);
    if (!BearerAuthorization.isValidToken(controlToken)) {
      throw new IllegalArgumentException("peer control token is invalid");
    }
    authorization = "Bearer " + controlToken;
    this.transport = Objects.requireNonNull(transport, "transport");
  }

  Response setStandby(boolean armed) throws IOException {
    byte[] body = ("{\"armed\":" + armed + "}").getBytes(StandardCharsets.UTF_8);
    return transport.request(endpoint("/api/v1/capture/arm"), "POST", authorization, body);
  }

  Response setStandbyWithRetries(boolean armed, int maximumAttempts, long retryDelayMillis)
      throws IOException, InterruptedException {
    return withRetries(() -> setStandby(armed), maximumAttempts, retryDelayMillis);
  }

  Response triggerReport() throws IOException {
    return transport.request(
        endpoint("/api/v1/capture/trigger-report"), "GET", authorization, new byte[0]);
  }

  private Response manifest(String localSessionId) throws IOException {
    requireSegment(localSessionId, "localSessionId");
    return transport.request(
        endpoint("/api/v1/sessions/" + localSessionId + "/manifest"),
        "HEAD",
        authorization,
        new byte[0]);
  }

  /**
   * Probes both immutable artifacts that make a peer clip reviewable.
   *
   * <p>The manifest is published last by the capture engine, but explicitly probing the expected
   * role-named MP4 keeps a corrupt, incomplete, or non-atomic peer implementation from being
   * admitted as one half of a paired session. HEAD requests avoid transferring either the
   * frame-rich manifest (which is commonly larger than the bounded control-response budget) or
   * the clip.
   */
  PublicationProbe publishedCapture(String localSessionId, String role) throws IOException {
    requireSegment(localSessionId, "localSessionId");
    if (!role.equals("down_the_line") && !role.equals("face_on")) {
      throw new IllegalArgumentException("role is invalid");
    }
    Response manifest = manifest(localSessionId);
    if (manifest.statusCode() != 200) {
      return new PublicationProbe(false, manifest.statusCode(), "manifest");
    }
    Response media =
        transport.request(
            endpoint("/api/v1/sessions/" + localSessionId + "/" + role + ".mp4"),
            "HEAD",
            authorization,
            new byte[0]);
    if (media.statusCode() != 200) {
      return new PublicationProbe(false, media.statusCode(), "media");
    }
    return new PublicationProbe(true, 200, "complete");
  }

  Response storeCoordinationRecord(PairedCoordinationRecord record) throws IOException {
    Objects.requireNonNull(record, "record");
    return transport.request(
        endpoint("/api/v1/coordination/" + record.sharedSessionId()),
        "POST",
        authorization,
        record.toJson().getBytes(StandardCharsets.UTF_8));
  }

  private URI endpoint(String path) {
    return origin.resolve(path);
  }

  @FunctionalInterface
  private interface Request {
    Response execute() throws IOException;
  }

  private static Response withRetries(
      Request request, int maximumAttempts, long retryDelayMillis)
      throws IOException, InterruptedException {
    if (maximumAttempts <= 0 || retryDelayMillis < 0) {
      throw new IllegalArgumentException("retry policy is invalid");
    }
    IOException lastFailure = null;
    Response lastResponse = null;
    for (int attempt = 1; attempt <= maximumAttempts; ++attempt) {
      try {
        lastResponse = request.execute();
        if (lastResponse.accepted() || lastResponse.statusCode() < 500) {
          return lastResponse;
        }
      } catch (IOException failure) {
        lastFailure = failure;
      }
      if (attempt < maximumAttempts && retryDelayMillis > 0) {
        Thread.sleep(retryDelayMillis);
      }
    }
    if (lastResponse != null) {
      return lastResponse;
    }
    throw Objects.requireNonNull(lastFailure, "lastFailure");
  }

  private static Response requestHttp(
      URI endpoint, String method, String authorization, byte[] body) throws IOException {
    HttpURLConnection connection = (HttpURLConnection) endpoint.toURL().openConnection();
    connection.setConnectTimeout(TIMEOUT_MILLIS);
    connection.setReadTimeout(TIMEOUT_MILLIS);
    connection.setInstanceFollowRedirects(false);
    connection.setRequestMethod(method);
    connection.setRequestProperty("Authorization", authorization);
    if (body.length > 0) {
      connection.setRequestProperty("Content-Type", "application/json; charset=utf-8");
      connection.setFixedLengthStreamingMode(body.length);
      connection.setDoOutput(true);
    }
    try {
      if (body.length > 0) {
        try (OutputStream output = connection.getOutputStream()) {
          output.write(body);
        }
      }
      int status = connection.getResponseCode();
      InputStream stream = status >= 400 ? connection.getErrorStream() : connection.getInputStream();
      String responseBody = "";
      if (stream != null) {
        try (InputStream input = stream) {
          byte[] contents = input.readNBytes(MAXIMUM_RESPONSE_BYTES + 1);
          if (contents.length > MAXIMUM_RESPONSE_BYTES || input.read() >= 0) {
            throw new IOException("peer lifecycle response exceeds configured bound");
          }
          responseBody = new String(contents, StandardCharsets.UTF_8);
        }
      }
      return new Response(status, responseBody);
    } finally {
      connection.disconnect();
    }
  }

  private static URI validateOrigin(String peerOrigin) {
    return PeerTransportSecurityPolicy.validateOrigin(
            peerOrigin, PeerTransportSecurityPolicy.Requirement.TRUSTED_LAN_DEMO_ALLOWED)
        .uri();
  }

  private static void requireSegment(String value, String name) {
    if (value == null || !value.matches("[A-Za-z0-9._-]{1,128}")) {
      throw new IllegalArgumentException(name + " is invalid");
    }
  }
}
