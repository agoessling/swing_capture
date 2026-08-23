package com.agoessling.swingcapture;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.LongSupplier;

/** Direct loopback-HTTP coverage for the production peer-clock transport contract. */
public final class PeerClockClientTest {
  private static final String EXPECTED_NODE_ID = "peer-node-1";

  private PeerClockClientTest() {}

  public static void main(String[] arguments) throws Exception {
    performsDirectPublicClockExchange();
    refusesRedirects();
    rejectsUntrustedIdentityMismatch();
    rejectsInvalidSampleTiming();
    rejectsOversizedResponse();
    validatesExactPeerOrigin();
  }

  private static void performsDirectPublicClockExchange() throws Exception {
    AtomicReference<String> requestMethod = new AtomicReference<>();
    AtomicReference<String> authorization = new AtomicReference<>();
    AtomicReference<String> accept = new AtomicReference<>();
    HttpServer server = server();
    server.createContext(
        "/api/v1/clock",
        request -> {
          requestMethod.set(request.getRequestMethod());
          authorization.set(request.getRequestHeaders().getFirst("Authorization"));
          accept.set(request.getRequestHeaders().getFirst("Accept"));
          respond(request, 200, "valid");
        });
    server.start();
    try {
      PeerClockClient client =
          client(server, times(1_000, 1_050), contents -> response(contents, EXPECTED_NODE_ID));
      PeerClockClient.Exchange exchange = client.exchange();
      check(exchange.peerNodeId().equals(EXPECTED_NODE_ID), "trusted peer identity retained");
      check(exchange.sample().coordinatorSendNs() == 1_000, "local send timestamp");
      check(exchange.sample().nodeReceiveNs() == 1_010, "peer receive timestamp");
      check(exchange.sample().nodeSendNs() == 1_020, "peer send timestamp");
      check(exchange.sample().coordinatorReceiveNs() == 1_050, "local receive timestamp");
      check(requestMethod.get().equals("GET"), "clock uses GET");
      check(authorization.get() == null, "public clock request sends no control credential");
      check("application/json".equals(accept.get()), "clock requests JSON");
    } finally {
      server.stop(0);
    }
  }

  private static void refusesRedirects() throws Exception {
    AtomicInteger redirectedRequests = new AtomicInteger();
    HttpServer server = server();
    server.createContext(
        "/api/v1/clock",
        request -> {
          request.getResponseHeaders().set("Location", "/redirected");
          respond(request, 307, "redirect refused");
        });
    server.createContext(
        "/redirected",
        request -> {
          redirectedRequests.incrementAndGet();
          respond(request, 200, "valid");
        });
    server.start();
    try {
      expectIOException(
          () ->
              client(server, times(1_000), contents -> response(contents, EXPECTED_NODE_ID))
                  .exchange(),
          "HTTP 307");
      check(redirectedRequests.get() == 0, "redirect target must not be contacted");
    } finally {
      server.stop(0);
    }
  }

  private static void rejectsUntrustedIdentityMismatch() throws Exception {
    HttpServer server = server();
    server.createContext("/api/v1/clock", request -> respond(request, 200, "wrong-node"));
    server.start();
    try {
      expectIOException(
          () ->
              client(server, times(1_000, 1_050), contents -> response(contents, "attacker"))
                  .exchange(),
          "another node");
    } finally {
      server.stop(0);
    }
  }

  private static void rejectsInvalidSampleTiming() throws Exception {
    HttpServer server = server();
    server.createContext("/api/v1/clock", request -> respond(request, 200, "invalid-time"));
    server.start();
    try {
      expectIOException(
          () ->
              client(server, times(2_000, 1_999), contents -> response(contents, EXPECTED_NODE_ID))
                  .exchange(),
          "invalid peer clock response");
      expectIOException(
          () ->
              client(
                      server,
                      times(2_000, 2_010),
                      contents -> new PeerClockClient.ClockResponse(EXPECTED_NODE_ID, 2_001, 2_020))
                  .exchange(),
          "invalid peer clock response");
    } finally {
      server.stop(0);
    }
  }

  private static void rejectsOversizedResponse() throws Exception {
    byte[] oversized = new byte[16 * 1024 + 1];
    HttpServer server = server();
    server.createContext(
        "/api/v1/clock",
        request -> {
          request.sendResponseHeaders(200, oversized.length);
          try (var output = request.getResponseBody()) {
            output.write(oversized);
          }
        });
    server.start();
    try {
      expectIOException(
          () ->
              client(server, times(1_000), contents -> response(contents, EXPECTED_NODE_ID))
                  .exchange(),
          "configured bound");
    } finally {
      server.stop(0);
    }
  }

  private static void validatesExactPeerOrigin() {
    for (String invalidOrigin :
        new String[] {
          "http://peer.invalid",
          "http://peer.invalid:8088/path",
          "http://user@peer.invalid:8088",
          "http://peer.invalid:8088?query"
        }) {
      expectIllegalArgument(
          () ->
              new PeerClockClient(
                  invalidOrigin,
                  EXPECTED_NODE_ID,
                  times(1),
                  contents -> response(contents, EXPECTED_NODE_ID)));
    }
    new PeerClockClient(
        "https://peer.invalid:8443",
        EXPECTED_NODE_ID,
        times(1),
        contents -> response(contents, EXPECTED_NODE_ID));
    expectIllegalArgument(
        () ->
            new PeerClockClient(
                "http://peer.invalid:8088",
                " ",
                times(1),
                contents -> response(contents, EXPECTED_NODE_ID)));
  }

  private static PeerClockClient client(
      HttpServer server,
      LongSupplier elapsedRealtimeNanos,
      PeerClockClient.ResponseParser parser) {
    return new PeerClockClient(
        "http://127.0.0.1:" + server.getAddress().getPort(),
        EXPECTED_NODE_ID,
        elapsedRealtimeNanos,
        parser);
  }

  private static PeerClockClient.ClockResponse response(byte[] contents, String nodeId) {
    check(contents.length > 0, "response body delivered to parser");
    return new PeerClockClient.ClockResponse(nodeId, 1_010, 1_020);
  }

  private static LongSupplier times(long... values) {
    AtomicInteger next = new AtomicInteger();
    return () -> {
      int index = next.getAndIncrement();
      if (index >= values.length) {
        throw new AssertionError("elapsed-realtime clock read too many times");
      }
      return values[index];
    };
  }

  private static HttpServer server() throws IOException {
    return HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
  }

  private static void respond(HttpExchange request, int status, String body) throws IOException {
    byte[] contents = body.getBytes(StandardCharsets.UTF_8);
    request.sendResponseHeaders(status, contents.length);
    try (var output = request.getResponseBody()) {
      output.write(contents);
    }
  }

  private static void expectIOException(ThrowingAction action, String messagePart) {
    try {
      action.run();
      throw new AssertionError("expected IOException containing " + messagePart);
    } catch (IOException expected) {
      check(
          expected.getMessage().contains(messagePart),
          "IOException diagnostic contains " + messagePart);
    } catch (Exception unexpected) {
      throw new AssertionError("expected IOException", unexpected);
    }
  }

  private static void expectIllegalArgument(Runnable action) {
    try {
      action.run();
      throw new AssertionError("expected IllegalArgumentException");
    } catch (IllegalArgumentException expected) {
      // Expected.
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
