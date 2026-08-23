package com.agoessling.swingcapture;

import com.agoessling.swingcapture.core.coordination.ClockExchangeSample;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URI;
import java.util.Objects;
import java.util.function.LongSupplier;

/** Performs one bounded four-timestamp exchange against a peer node's public clock hint. */
final class PeerClockClient {
  private static final int TIMEOUT_MILLIS = 1_500;
  private static final int MAXIMUM_RESPONSE_BYTES = 16 * 1024;

  record Exchange(String peerNodeId, ClockExchangeSample sample) {
    Exchange {
      Objects.requireNonNull(peerNodeId, "peerNodeId");
      Objects.requireNonNull(sample, "sample");
      if (peerNodeId.isBlank()) {
        throw new IllegalArgumentException("peerNodeId cannot be blank");
      }
    }
  }

  record ClockResponse(String peerNodeId, long peerReceiveNanos, long peerSendNanos) {
    ClockResponse {
      Objects.requireNonNull(peerNodeId, "peerNodeId");
      if (peerNodeId.isBlank() || peerReceiveNanos < 0 || peerSendNanos < 0) {
        throw new IllegalArgumentException("invalid peer clock response fields");
      }
    }
  }

  @FunctionalInterface
  interface ResponseParser {
    ClockResponse parse(byte[] contents) throws IOException;
  }

  private final URI endpoint;
  private final String expectedPeerNodeId;
  private final LongSupplier elapsedRealtimeNanos;
  private final ResponseParser responseParser;

  PeerClockClient(
      String peerOrigin,
      String expectedPeerNodeId,
      LongSupplier elapsedRealtimeNanos,
      ResponseParser responseParser) {
    Objects.requireNonNull(peerOrigin, "peerOrigin");
    this.expectedPeerNodeId = Objects.requireNonNull(expectedPeerNodeId, "expectedPeerNodeId");
    this.elapsedRealtimeNanos =
        Objects.requireNonNull(elapsedRealtimeNanos, "elapsedRealtimeNanos");
    this.responseParser = Objects.requireNonNull(responseParser, "responseParser");
    if (expectedPeerNodeId.isBlank()) {
      throw new IllegalArgumentException("expected peer node ID cannot be blank");
    }
    endpoint =
        PeerTransportSecurityPolicy.validateOrigin(
                peerOrigin, PeerTransportSecurityPolicy.Requirement.TRUSTED_LAN_DEMO_ALLOWED)
            .uri()
            .resolve("/api/v1/clock");
  }

  Exchange exchange() throws IOException {
    long localSendNanos = elapsedRealtimeNanos.getAsLong();
    HttpURLConnection connection = (HttpURLConnection) endpoint.toURL().openConnection();
    connection.setConnectTimeout(TIMEOUT_MILLIS);
    connection.setReadTimeout(TIMEOUT_MILLIS);
    connection.setInstanceFollowRedirects(false);
    connection.setUseCaches(false);
    connection.setRequestMethod("GET");
    connection.setRequestProperty("Accept", "application/json");
    try {
      int status = connection.getResponseCode();
      if (status != 200) {
        throw new IOException("peer clock request returned HTTP " + status);
      }
      byte[] contents;
      try (InputStream input = connection.getInputStream()) {
        contents = input.readNBytes(MAXIMUM_RESPONSE_BYTES + 1);
        if (contents.length > MAXIMUM_RESPONSE_BYTES || input.read() >= 0) {
          throw new IOException("peer clock response exceeds configured bound");
        }
      }
      long localReceiveNanos = elapsedRealtimeNanos.getAsLong();
      try {
        ClockResponse response = responseParser.parse(contents);
        if (response == null) {
          throw new IOException("peer clock response parser returned no result");
        }
        if (!expectedPeerNodeId.equals(response.peerNodeId())) {
          throw new IOException("peer clock response belongs to another node");
        }
        return new Exchange(
            expectedPeerNodeId,
            new ClockExchangeSample(
                localSendNanos,
                response.peerReceiveNanos(),
                response.peerSendNanos(),
                localReceiveNanos));
      } catch (ArithmeticException | IllegalArgumentException invalid) {
        throw new IOException("invalid peer clock response", invalid);
      }
    } finally {
      connection.disconnect();
    }
  }

}
