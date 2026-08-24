package com.agoessling.swingcapture;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.DatagramSocket;
import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.SocketTimeoutException;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.function.LongSupplier;

/** Bounded application-level probes used for one directed or bidirectional pair-health round. */
final class PairNetworkHealthClient {
  static final int CLOCK_ATTEMPTS = 3;
  static final int MAXIMUM_TRANSFER_BYTES = 1024 * 1024;
  static final int MAXIMUM_REVERSE_RESPONSE_BYTES =
      PairNetworkHealthWireJson.MAXIMUM_DIRECTION_BYTES;
  private static final int TIMEOUT_MILLIS = 1_500;
  private static final int REVERSE_READ_TIMEOUT_MILLIS = 7_000;
  private static final String JAVASCRIPT_CONTENT_TYPE = "text/javascript; charset=utf-8";

  @FunctionalInterface
  interface DirectionParser {
    PairNetworkHealthPolicy.DirectionEvidence parse(byte[] contents) throws IOException;
  }

  private final LongSupplier elapsedRealtimeNanos;
  private final PeerClockClient.ResponseParser clockParser;

  PairNetworkHealthClient(
      LongSupplier elapsedRealtimeNanos, PeerClockClient.ResponseParser clockParser) {
    this.elapsedRealtimeNanos =
        Objects.requireNonNull(elapsedRealtimeNanos, "elapsedRealtimeNanos");
    this.clockParser = Objects.requireNonNull(clockParser, "clockParser");
  }

  PairNetworkHealthPolicy.DirectionEvidence measureDirection(
      String destinationOrigin, String expectedDestinationNodeId) {
    ArrayList<Long> roundTrips = new ArrayList<>();
    int timeouts = 0;
    for (int attempt = 0; attempt < CLOCK_ATTEMPTS; ++attempt) {
      try {
        roundTrips.add(
            new PeerClockClient(
                    destinationOrigin,
                    expectedDestinationNodeId,
                    elapsedRealtimeNanos,
                    clockParser)
                .exchange()
                .sample()
                .networkRoundTripNs());
      } catch (SocketTimeoutException timeout) {
        ++timeouts;
      } catch (IOException unavailable) {
        // A bounded failure is retained by the attempt/success counts.
      }
    }

    Transfer transfer = transfer(destinationOrigin);
    return new PairNetworkHealthPolicy.DirectionEvidence(
        CLOCK_ATTEMPTS,
        roundTrips.size(),
        timeouts,
        roundTrips,
        transfer.bytes(),
        transfer.durationNanos(),
        transfer.complete());
  }

  PairNetworkHealthPolicy.DirectionEvidence requestReverseDirection(
      String peerOrigin,
      String peerControlToken,
      byte[] requestBody,
      DirectionParser responseParser)
      throws IOException {
    Objects.requireNonNull(peerControlToken, "peerControlToken");
    Objects.requireNonNull(requestBody, "requestBody");
    Objects.requireNonNull(responseParser, "responseParser");
    if (peerControlToken.isBlank()) {
      throw new IllegalArgumentException("peer control token cannot be blank");
    }
    if (requestBody.length == 0
        || requestBody.length > PairNetworkHealthWireJson.MAXIMUM_REVERSE_REQUEST_BYTES) {
      throw new IllegalArgumentException("reverse request body has invalid length");
    }
    URI endpoint = validatedOrigin(peerOrigin).resolve(NodeHttpProtocol.NETWORK_HEALTH_REVERSE_PATH);
    HttpURLConnection connection = (HttpURLConnection) endpoint.toURL().openConnection();
    connection.setConnectTimeout(TIMEOUT_MILLIS);
    connection.setReadTimeout(REVERSE_READ_TIMEOUT_MILLIS);
    connection.setInstanceFollowRedirects(false);
    connection.setUseCaches(false);
    connection.setDoOutput(true);
    connection.setFixedLengthStreamingMode(requestBody.length);
    connection.setRequestMethod("POST");
    connection.setRequestProperty("Accept", "application/json");
    connection.setRequestProperty("Content-Type", "application/json; charset=utf-8");
    connection.setRequestProperty("Authorization", "Bearer " + peerControlToken);
    try {
      try (OutputStream output = connection.getOutputStream()) {
        output.write(requestBody);
      }
      if (connection.getResponseCode() != 200) {
        throw new IOException(
            "peer reverse network-health request returned HTTP " + connection.getResponseCode());
      }
      requireJsonContentType(connection);
      PairNetworkHealthPolicy.DirectionEvidence parsed =
          responseParser.parse(readStrictBody(connection, MAXIMUM_REVERSE_RESPONSE_BYTES));
      if (parsed == null) {
        throw new IOException("peer reverse network-health parser returned no result");
      }
      return parsed;
    } finally {
      connection.disconnect();
    }
  }

  static String callbackOrigin(String peerOrigin, int listenerPort) throws IOException {
    URI peer = validatedOrigin(peerOrigin);
    try (DatagramSocket socket = new DatagramSocket()) {
      socket.connect(new InetSocketAddress(peer.getHost(), peer.getPort()));
      String localAddress = socket.getLocalAddress().getHostAddress();
      if (localAddress.contains(":") || localAddress.equals("0.0.0.0")) {
        throw new IOException("no route-selected IPv4 callback address is available");
      }
      return "http://" + localAddress + ":" + listenerPort;
    }
  }

  private Transfer transfer(String destinationOrigin) {
    HttpURLConnection connection = null;
    long started = 0;
    try {
      URI endpoint = validatedOrigin(destinationOrigin).resolve("/app.js");
      connection = (HttpURLConnection) endpoint.toURL().openConnection();
      connection.setConnectTimeout(TIMEOUT_MILLIS);
      connection.setReadTimeout(TIMEOUT_MILLIS);
      connection.setInstanceFollowRedirects(false);
      connection.setUseCaches(false);
      connection.setRequestMethod("GET");
      connection.setRequestProperty("Accept", JAVASCRIPT_CONTENT_TYPE);
      started = elapsedRealtimeNanos.getAsLong();
      if (connection.getResponseCode() != 200
          || !JAVASCRIPT_CONTENT_TYPE.equalsIgnoreCase(connection.getContentType())) {
        return Transfer.incomplete();
      }
      long declaredLengthLong = connection.getContentLengthLong();
      if (declaredLengthLong <= 0 || declaredLengthLong > MAXIMUM_TRANSFER_BYTES) {
        return Transfer.incomplete();
      }
      int declaredLength = Math.toIntExact(declaredLengthLong);
      byte[] contents;
      try (InputStream input = connection.getInputStream()) {
        contents = input.readNBytes(declaredLength + 1);
        if (contents.length != declaredLength) {
          return Transfer.incomplete();
        }
      }
      long duration = Math.subtractExact(elapsedRealtimeNanos.getAsLong(), started);
      if (duration <= 0) {
        return Transfer.incomplete();
      }
      return new Transfer(contents.length, duration, true);
    } catch (IOException | ArithmeticException unavailable) {
      return Transfer.incomplete();
    } finally {
      if (connection != null) {
        connection.disconnect();
      }
    }
  }

  private static URI validatedOrigin(String origin) {
    return PeerTransportSecurityPolicy.validateOrigin(
            origin, PeerTransportSecurityPolicy.Requirement.TRUSTED_LAN_DEMO_ALLOWED)
        .uri();
  }

  private static void requireJsonContentType(HttpURLConnection connection) throws IOException {
    String contentType = connection.getContentType();
    if (contentType == null || !contentType.equalsIgnoreCase("application/json; charset=utf-8")) {
      throw new IOException("peer reverse network-health response has invalid content type");
    }
  }

  private static byte[] readStrictBody(HttpURLConnection connection, int maximumBytes)
      throws IOException {
    long declaredLengthLong = connection.getContentLengthLong();
    if (declaredLengthLong <= 0 || declaredLengthLong > maximumBytes) {
      throw new IOException("peer reverse network-health response has invalid length");
    }
    int declaredLength = Math.toIntExact(declaredLengthLong);
    try (InputStream input = connection.getInputStream()) {
      byte[] contents = input.readNBytes(declaredLength + 1);
      if (contents.length != declaredLength) {
        throw new IOException("peer reverse network-health response length disagrees");
      }
      return contents;
    }
  }

  private record Transfer(long bytes, long durationNanos, boolean complete) {
    private static Transfer incomplete() {
      return new Transfer(0, 0, false);
    }
  }
}
