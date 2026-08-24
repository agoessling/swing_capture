package com.agoessling.swingcapture;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.LongSupplier;

/** Loopback coverage for strict, bounded application-level pair-health transport. */
public final class PairNetworkHealthClientTest {
  private static final String NODE_ID = "peer-node";

  private PairNetworkHealthClientTest() {}

  public static void main(String[] arguments) throws Exception {
    measuresClocksAndRepresentativeTransfer();
    incompleteOrRedirectedTransferIsRetainedAsEvidence();
    reverseProbeUsesBearerAndRefusesRedirects();
    reverseProbeRejectsUnboundedOrMissingResponses();
    derivesRouteSelectedCallbackOrigin();
  }

  private static void measuresClocksAndRepresentativeTransfer() throws Exception {
    byte[] script = "representative javascript".getBytes(StandardCharsets.UTF_8);
    HttpServer server = server();
    server.createContext("/api/v1/clock", request -> respond(request, 200, "application/json", "x"));
    server.createContext(
        "/app.js",
        request -> respond(request, 200, "text/javascript; charset=utf-8", script));
    server.start();
    try {
      PairNetworkHealthPolicy.DirectionEvidence evidence =
          client(times(1_000, 1_100, 2_000, 2_100, 3_000, 3_100, 4_000, 5_000))
              .measureDirection(origin(server), NODE_ID);
      check(evidence.attempts() == 3 && evidence.successes() == 3, "three clock attempts succeed");
      check(evidence.timeouts() == 0, "no invented timeout");
      check(evidence.roundTripNanos().equals(List.of(90L, 90L, 90L)), "network RTT retained");
      check(evidence.transferComplete(), "representative transfer completes");
      check(evidence.transferBytes() == script.length, "transfer byte count exact");
      check(evidence.transferDurationNanos() == 1_000, "transfer duration uses monotonic clock");
    } finally {
      server.stop(0);
    }
  }

  private static void incompleteOrRedirectedTransferIsRetainedAsEvidence() throws Exception {
    AtomicInteger redirected = new AtomicInteger();
    HttpServer server = server();
    server.createContext("/api/v1/clock", request -> respond(request, 500, "application/json", "x"));
    server.createContext(
        "/app.js",
        request -> {
          request.getResponseHeaders().set("Location", "/redirected.js");
          respond(request, 307, "text/plain", "redirect");
        });
    server.createContext(
        "/redirected.js",
        request -> {
          redirected.incrementAndGet();
          respond(request, 200, "text/javascript; charset=utf-8", "unsafe");
        });
    server.start();
    try {
      PairNetworkHealthPolicy.DirectionEvidence evidence =
          client(times(1_000, 2_000, 3_000, 4_000)).measureDirection(origin(server), NODE_ID);
      check(evidence.successes() == 0, "clock failures retained");
      check(!evidence.transferComplete(), "redirect is not representative success");
      check(redirected.get() == 0, "redirect target is not contacted");
    } finally {
      server.stop(0);
    }
  }

  private static void reverseProbeUsesBearerAndRefusesRedirects() throws Exception {
    AtomicReference<String> authorization = new AtomicReference<>();
    AtomicReference<String> method = new AtomicReference<>();
    AtomicInteger redirected = new AtomicInteger();
    HttpServer server = server();
    server.createContext(
        NodeHttpProtocol.NETWORK_HEALTH_REVERSE_PATH,
        request -> {
          authorization.set(request.getRequestHeaders().getFirst("Authorization"));
          method.set(request.getRequestMethod());
          request.getRequestBody().readAllBytes();
          respond(request, 200, "application/json; charset=utf-8", "direction");
        });
    server.start();
    try {
      PairNetworkHealthPolicy.DirectionEvidence expected = goodDirection();
      PairNetworkHealthPolicy.DirectionEvidence parsed =
          client(times(1))
              .requestReverseDirection(
                  origin(server), "secret", "request".getBytes(StandardCharsets.UTF_8), body -> expected);
      check(parsed.equals(expected), "strict response parser result returned");
      check("POST".equals(method.get()), "reverse measurement uses POST");
      check("Bearer secret".equals(authorization.get()), "reverse measurement uses configured bearer");
    } finally {
      server.stop(0);
    }

    HttpServer redirectServer = server();
    redirectServer.createContext(
        NodeHttpProtocol.NETWORK_HEALTH_REVERSE_PATH,
        request -> {
          request.getResponseHeaders().set("Location", "/redirected");
          respond(request, 307, "text/plain", "redirect");
        });
    redirectServer.createContext(
        "/redirected",
        request -> {
          redirected.incrementAndGet();
          respond(request, 200, "application/json; charset=utf-8", "unsafe");
        });
    redirectServer.start();
    try {
      expectIOException(
          () ->
              client(times(1))
                  .requestReverseDirection(
                      origin(redirectServer),
                      "secret",
                      "request".getBytes(StandardCharsets.UTF_8),
                      body -> goodDirection()));
      check(redirected.get() == 0, "reverse redirect target is not contacted");
    } finally {
      redirectServer.stop(0);
    }
  }

  private static void reverseProbeRejectsUnboundedOrMissingResponses() throws Exception {
    AtomicInteger requests = new AtomicInteger();
    HttpServer server = server();
    server.createContext(
        NodeHttpProtocol.NETWORK_HEALTH_REVERSE_PATH,
        request -> {
          requests.incrementAndGet();
          request.getRequestBody().readAllBytes();
          respond(request, 200, "application/json; charset=utf-8", "direction");
        });
    server.start();
    try {
      expectIllegalArgument(
          () ->
              client(times(1))
                  .requestReverseDirection(origin(server), "secret", new byte[0], body -> goodDirection()));
      expectIllegalArgument(
          () ->
              client(times(1))
                  .requestReverseDirection(
                      origin(server),
                      "secret",
                      new byte[PairNetworkHealthWireJson.MAXIMUM_REVERSE_REQUEST_BYTES + 1],
                      body -> goodDirection()));
      check(requests.get() == 0, "invalid request bodies are rejected before network access");

      expectIOException(
          () ->
              client(times(1))
                  .requestReverseDirection(
                      origin(server),
                      "secret",
                      "request".getBytes(StandardCharsets.UTF_8),
                      body -> null));
      check(requests.get() == 1, "missing parser result is rejected after one bounded request");
    } finally {
      server.stop(0);
    }
  }

  private static void derivesRouteSelectedCallbackOrigin() throws Exception {
    HttpServer server = server();
    server.start();
    try {
      check(
          PairNetworkHealthClient.callbackOrigin(origin(server), 8088)
              .equals("http://127.0.0.1:8088"),
          "callback uses the route-selected local IPv4 address and fixed listener port");
    } finally {
      server.stop(0);
    }
  }

  private static PairNetworkHealthClient client(LongSupplier clock) {
    return new PairNetworkHealthClient(
        clock, body -> new PeerClockClient.ClockResponse(NODE_ID, 1_010, 1_020));
  }

  private static PairNetworkHealthPolicy.DirectionEvidence goodDirection() {
    return new PairNetworkHealthPolicy.DirectionEvidence(
        3, 3, 0, List.of(10L, 20L, 30L), 100, 1_000, true);
  }

  private static LongSupplier times(long... values) {
    AtomicInteger index = new AtomicInteger();
    return () -> {
      int current = index.getAndIncrement();
      if (current >= values.length) {
        throw new AssertionError("monotonic clock read too many times");
      }
      return values[current];
    };
  }

  private static HttpServer server() throws IOException {
    return HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
  }

  private static String origin(HttpServer server) {
    return "http://127.0.0.1:" + server.getAddress().getPort();
  }

  private static void respond(HttpExchange request, int status, String type, String body)
      throws IOException {
    respond(request, status, type, body.getBytes(StandardCharsets.UTF_8));
  }

  private static void respond(HttpExchange request, int status, String type, byte[] body)
      throws IOException {
    request.getResponseHeaders().set("Content-Type", type);
    request.sendResponseHeaders(status, body.length);
    try (var output = request.getResponseBody()) {
      output.write(body);
    }
  }

  private static void expectIOException(ThrowingAction action) {
    try {
      action.run();
      throw new AssertionError("expected IOException");
    } catch (IOException expected) {
      // Expected.
    } catch (Exception unexpected) {
      throw new AssertionError("expected IOException", unexpected);
    }
  }

  private static void expectIllegalArgument(ThrowingAction action) {
    try {
      action.run();
      throw new AssertionError("expected IllegalArgumentException");
    } catch (IllegalArgumentException expected) {
      // Expected.
    } catch (Exception unexpected) {
      throw new AssertionError("expected IllegalArgumentException", unexpected);
    }
  }

  @FunctionalInterface
  private interface ThrowingAction {
    void run() throws Exception;
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
