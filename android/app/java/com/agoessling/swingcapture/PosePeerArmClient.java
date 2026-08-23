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
import java.util.function.BooleanSupplier;

/** Bounded authenticated leader-to-peer pose arm request. */
public final class PosePeerArmClient {
  private static final int TIMEOUT_MILLIS = 1_500;
  private static final int MAXIMUM_RESPONSE_BYTES = 16 * 1024;
  static final String POSE_READY_HEADER = "X-Swing-Capture-Pose-Ready";

  static final class ArmPollingCancelledException extends IOException {
    ArmPollingCancelledException() {
      super("peer arm readiness polling was cancelled");
    }
  }

  static String sharedSessionIdForLocalTrigger(
      NodeCoordinationState.TriggerReport report, String localSessionId) {
    Objects.requireNonNull(report, "report");
    if (!report.localSessionId().equals(localSessionId)) {
      throw new IllegalArgumentException(
          "the local trigger does not match the coordinated trigger report");
    }
    return report.sharedSessionId();
  }

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

  /** A terminal impact selected by the leader for an already-shared high-speed attempt. */
  public record ImpactTrigger(
      int schemaVersion,
      String sharedSessionId,
      String leaderNodeId,
      long leaderTriggerElapsedRealtimeNanos,
      String targetPeerNodeId,
      long mappedPeerTriggerElapsedRealtimeNanos,
      long mappingUncertaintyNanos,
      long mappingAgeAtSendNanos,
      long minimumRoundTripNanos,
      long maximumRoundTripNanos,
      int sampleCount) {
    public ImpactTrigger(
        String sharedSessionId, String leaderNodeId, long leaderTriggerElapsedRealtimeNanos) {
      this(
          1,
          sharedSessionId,
          leaderNodeId,
          leaderTriggerElapsedRealtimeNanos,
          "",
          0,
          -1,
          -1,
          -1,
          -1,
          0);
    }

    public ImpactTrigger {
      NodeCoordinationState.validateSharedSessionId(sharedSessionId);
      requireIdentifier(leaderNodeId, "leaderNodeId");
      if (leaderTriggerElapsedRealtimeNanos <= 0) {
        throw new IllegalArgumentException("leader trigger timestamp must be positive");
      }
      if (schemaVersion == 1) {
        if (!targetPeerNodeId.isEmpty()
            || mappedPeerTriggerElapsedRealtimeNanos != 0
            || mappingUncertaintyNanos != -1
            || mappingAgeAtSendNanos != -1
            || minimumRoundTripNanos != -1
            || maximumRoundTripNanos != -1
            || sampleCount != 0) {
          throw new IllegalArgumentException("schema 1 impact cannot contain clock mapping");
        }
      } else if (schemaVersion == 2) {
        requireIdentifier(targetPeerNodeId, "targetPeerNodeId");
        if (mappedPeerTriggerElapsedRealtimeNanos <= 0
            || mappingUncertaintyNanos < 0
            || mappingAgeAtSendNanos < 0
            || minimumRoundTripNanos < 0
            || maximumRoundTripNanos < minimumRoundTripNanos
            || sampleCount < 3) {
          throw new IllegalArgumentException("schema 2 impact clock mapping is invalid");
        }
      } else {
        throw new IllegalArgumentException("unsupported impact schema version");
      }
    }

    static ImpactTrigger mapped(
        String sharedSessionId,
        String leaderNodeId,
        long leaderTriggerElapsedRealtimeNanos,
        String targetPeerNodeId,
        long mappedPeerTriggerElapsedRealtimeNanos,
        long mappingUncertaintyNanos,
        long mappingAgeAtSendNanos,
        long minimumRoundTripNanos,
        long maximumRoundTripNanos,
        int sampleCount) {
      return new ImpactTrigger(
          2,
          sharedSessionId,
          leaderNodeId,
          leaderTriggerElapsedRealtimeNanos,
          targetPeerNodeId,
          mappedPeerTriggerElapsedRealtimeNanos,
          mappingUncertaintyNanos,
          mappingAgeAtSendNanos,
          minimumRoundTripNanos,
          maximumRoundTripNanos,
          sampleCount);
    }

    boolean hasClockMapping() {
      return schemaVersion == 2;
    }

    ImpactTrigger withoutClockMapping() {
      return new ImpactTrigger(sharedSessionId, leaderNodeId, leaderTriggerElapsedRealtimeNanos);
    }

    public byte[] requestBody() {
      String json;
      if (schemaVersion == 1) {
        json =
            "{\"schema_version\":1,\"shared_session_id\":\""
                + escapeJson(sharedSessionId)
                + "\",\"leader_node_id\":\""
                + escapeJson(leaderNodeId)
                + "\",\"leader_trigger_elapsed_realtime_ns\":\""
                + leaderTriggerElapsedRealtimeNanos
                + "\"}";
      } else {
        json =
            "{\"schema_version\":2,\"shared_session_id\":\""
                + escapeJson(sharedSessionId)
                + "\",\"leader_node_id\":\""
                + escapeJson(leaderNodeId)
                + "\",\"leader_trigger_elapsed_realtime_ns\":\""
                + leaderTriggerElapsedRealtimeNanos
                + "\",\"target_peer_node_id\":\""
                + escapeJson(targetPeerNodeId)
                + "\",\"mapped_peer_trigger_elapsed_realtime_ns\":\""
                + mappedPeerTriggerElapsedRealtimeNanos
                + "\",\"mapping_uncertainty_ns\":\""
                + mappingUncertaintyNanos
                + "\",\"mapping_age_at_send_ns\":\""
                + mappingAgeAtSendNanos
                + "\",\"minimum_round_trip_ns\":\""
                + minimumRoundTripNanos
                + "\",\"maximum_round_trip_ns\":\""
                + maximumRoundTripNanos
                + "\",\"sample_count\":"
                + sampleCount
                + "}";
      }
      return json.getBytes(StandardCharsets.UTF_8);
    }
  }

  public record Response(int statusCode, String body, boolean poseReady) {
    public Response(int statusCode, String body) {
      this(statusCode, body, false);
    }

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

  private final URI armEndpoint;
  private final URI impactEndpoint;
  private final String authorization;
  private final Transport transport;

  public PosePeerArmClient(String peerOrigin, String controlToken) {
    this(peerOrigin, controlToken, PosePeerArmClient::postHttp);
  }

  PosePeerArmClient(String peerOrigin, String controlToken, Transport transport) {
    armEndpoint = endpoint(peerOrigin, "/api/v1/capture/pose-arm");
    impactEndpoint = endpoint(peerOrigin, "/api/v1/capture/pose-impact");
    if (!BearerAuthorization.isValidToken(controlToken)) {
      throw new IllegalArgumentException("peer control token is invalid");
    }
    authorization = "Bearer " + controlToken;
    this.transport = Objects.requireNonNull(transport, "transport");
  }

  public Response arm(Candidate candidate) throws IOException {
    Objects.requireNonNull(candidate, "candidate");
    return transport.post(armEndpoint, authorization, candidate.requestBody());
  }

  /** Retries only transport and server failures while preserving terminal client rejections. */
  public Response armWithRetries(Candidate candidate, int maximumAttempts, long retryDelayMillis)
      throws IOException, InterruptedException {
    Objects.requireNonNull(candidate, "candidate");
    return withRetries(() -> arm(candidate), maximumAttempts, retryDelayMillis);
  }

  /**
   * Repeats the idempotent arm request until the peer confirms that its encoded pre-roll is ready.
   *
   * <p>HTTP 202 admits the shared session but deliberately does not mean that the peer can accept
   * an impact yet. Server and transport failures have their own small budget, while successful
   * pending responses use the readiness-poll budget.
   */
  public Response armUntilReady(
      Candidate candidate,
      int maximumTransientAttempts,
      int maximumPendingResponses,
      long retryDelayMillis)
      throws IOException, InterruptedException {
    return armUntilReady(
        candidate,
        maximumTransientAttempts,
        maximumPendingResponses,
        retryDelayMillis,
        () -> true);
  }

  Response armUntilReady(
      Candidate candidate,
      int maximumTransientAttempts,
      int maximumPendingResponses,
      long retryDelayMillis,
      BooleanSupplier continuePolling)
      throws IOException, InterruptedException {
    Objects.requireNonNull(candidate, "candidate");
    Objects.requireNonNull(continuePolling, "continuePolling");
    if (maximumTransientAttempts <= 0) {
      throw new IllegalArgumentException("maximumTransientAttempts must be positive");
    }
    if (maximumPendingResponses <= 0) {
      throw new IllegalArgumentException("maximumPendingResponses must be positive");
    }
    if (retryDelayMillis < 0) {
      throw new IllegalArgumentException("retryDelayMillis cannot be negative");
    }
    int transientAttempts = 0;
    int pendingResponses = 0;
    IOException lastTransportFailure = null;
    Response lastResponse = null;
    while (transientAttempts < maximumTransientAttempts
        && pendingResponses < maximumPendingResponses) {
      if (!continuePolling.getAsBoolean()) {
        throw new ArmPollingCancelledException();
      }
      try {
        lastResponse = arm(candidate);
        if (!continuePolling.getAsBoolean()) {
          throw new ArmPollingCancelledException();
        }
        if (lastResponse.accepted() && lastResponse.poseReady()) {
          return lastResponse;
        }
        if (lastResponse.accepted()) {
          ++pendingResponses;
        } else if (lastResponse.statusCode() >= 500) {
          ++transientAttempts;
        } else {
          return lastResponse;
        }
      } catch (ArmPollingCancelledException cancelled) {
        throw cancelled;
      } catch (IOException failure) {
        lastTransportFailure = failure;
        ++transientAttempts;
      }
      if (transientAttempts < maximumTransientAttempts
          && pendingResponses < maximumPendingResponses
          && retryDelayMillis > 0) {
        if (!continuePolling.getAsBoolean()) {
          throw new ArmPollingCancelledException();
        }
        Thread.sleep(retryDelayMillis);
      }
    }
    if (lastResponse != null) {
      return lastResponse;
    }
    throw Objects.requireNonNull(lastTransportFailure, "lastTransportFailure");
  }

  public Response triggerImpact(ImpactTrigger trigger) throws IOException {
    Objects.requireNonNull(trigger, "trigger");
    return transport.post(impactEndpoint, authorization, trigger.requestBody());
  }

  /** Retries transient peer-impact delivery while preserving non-retriable client failures. */
  public Response triggerImpactWithRetries(
      ImpactTrigger trigger, int maximumAttempts, long retryDelayMillis)
      throws IOException, InterruptedException {
    Objects.requireNonNull(trigger, "trigger");
    return withRetries(() -> triggerImpact(trigger), maximumAttempts, retryDelayMillis);
  }

  @FunctionalInterface
  private interface Request {
    Response execute() throws IOException;
  }

  private static Response withRetries(
      Request request, int maximumAttempts, long retryDelayMillis)
      throws IOException, InterruptedException {
    Objects.requireNonNull(request, "request");
    if (maximumAttempts <= 0) {
      throw new IllegalArgumentException("maximumAttempts must be positive");
    }
    if (retryDelayMillis < 0) {
      throw new IllegalArgumentException("retryDelayMillis cannot be negative");
    }
    IOException lastTransportFailure = null;
    Response lastResponse = null;
    for (int attempt = 1; attempt <= maximumAttempts; ++attempt) {
      try {
        lastResponse = request.execute();
        if (lastResponse.accepted() || lastResponse.statusCode() < 500) {
          return lastResponse;
        }
      } catch (IOException failure) {
        lastTransportFailure = failure;
      }
      if (attempt < maximumAttempts && retryDelayMillis > 0) {
        Thread.sleep(retryDelayMillis);
      }
    }
    if (lastResponse != null) {
      return lastResponse;
    }
    throw Objects.requireNonNull(lastTransportFailure, "lastTransportFailure");
  }

  /** Falls back to schema 1 only when a peer rejects schema-2 mapping before accepting a trigger. */
  public Response triggerImpactWithRetriesAndMappingFallback(
      ImpactTrigger trigger, int maximumAttempts, long retryDelayMillis)
      throws IOException, InterruptedException {
    Response response = triggerImpactWithRetries(trigger, maximumAttempts, retryDelayMillis);
    if (trigger.hasClockMapping() && response.statusCode() == 400) {
      return triggerImpactWithRetries(
          trigger.withoutClockMapping(), maximumAttempts, retryDelayMillis);
    }
    return response;
  }

  URI endpoint() {
    return armEndpoint;
  }

  URI impactEndpoint() {
    return impactEndpoint;
  }

  private static URI endpoint(String peerOrigin, String path) {
    return PeerTransportSecurityPolicy.validateOrigin(
            peerOrigin, PeerTransportSecurityPolicy.Requirement.TRUSTED_LAN_DEMO_ALLOWED)
        .uri()
        .resolve(path);
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
      return new Response(
          status,
          responseBody,
          "true".equalsIgnoreCase(connection.getHeaderField(POSE_READY_HEADER)));
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
