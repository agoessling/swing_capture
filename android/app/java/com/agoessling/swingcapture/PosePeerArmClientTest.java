package com.agoessling.swingcapture;

import com.sun.net.httpserver.HttpServer;
import com.agoessling.swingcapture.node.BearerAuthorization;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

public final class PosePeerArmClientTest {
  private PosePeerArmClientTest() {}

  public static void main(String[] args) throws Exception {
    String token = BearerAuthorization.generate(new SecureRandom());
    AtomicReference<String> request = new AtomicReference<>();
    PosePeerArmClient client =
        new PosePeerArmClient(
            "http://192.168.1.20:8088",
            token,
            (endpoint, authorization, body) -> {
              check(
                  endpoint
                      .toString()
                      .equals("http://192.168.1.20:8088/api/v1/capture/pose-arm"),
                  "endpoint");
              check(authorization.equals("Bearer " + token), "authorization");
              request.set(new String(body, StandardCharsets.UTF_8));
              return new PosePeerArmClient.Response(202, "{}");
            });
    PosePeerArmClient.Response response =
        client.arm(new PosePeerArmClient.Candidate("pose-1", "leader-node", 123, 0.9, 0.75));
    check(response.accepted(), "accepted response");
    check(
        request.get().equals(
            "{\"schema_version\":1,\"shared_session_id\":\"pose-1\","
                + "\"leader_node_id\":\"leader-node\","
                + "\"candidate_elapsed_realtime_ns\":\"123\","
                + "\"person_confidence\":0.9,\"address_confidence\":0.75}"),
        "canonical request");

    expectFailure(() -> new PosePeerArmClient("https://host:8088", token), "https origin");
    expectFailure(() -> new PosePeerArmClient("http://host:8088/path", token), "origin path");
    expectFailure(() -> new PosePeerArmClient("http://host", token), "missing port");
    expectFailure(() -> new PosePeerArmClient("http://host:8088", "bad"), "bad token");
    expectFailure(
        () -> new PosePeerArmClient.Candidate("pose-1", "leader", 0, 0.9, 0.8),
        "zero timestamp");
    expectFailure(
        () -> new PosePeerArmClient.Candidate("pose-1", "leader", 1, 1.1, 0.8),
        "invalid confidence");
    check(!new PosePeerArmClient.Response(409, "busy").accepted(), "rejected response");
    rejectsRedirectsWithoutForwardingCredential(token);
  }

  private static void rejectsRedirectsWithoutForwardingCredential(String token) throws Exception {
    HttpServer server =
        HttpServer.create(new InetSocketAddress(InetAddress.getLoopbackAddress(), 0), 0);
    AtomicReference<String> receivedAuthorization = new AtomicReference<>();
    AtomicInteger redirectedRequests = new AtomicInteger();
    server.createContext(
        "/api/v1/capture/pose-arm",
        exchange -> {
          receivedAuthorization.set(exchange.getRequestHeaders().getFirst("Authorization"));
          exchange.getRequestBody().readAllBytes();
          byte[] body = "redirect refused".getBytes(StandardCharsets.UTF_8);
          exchange
              .getResponseHeaders()
              .add(
                  "Location",
                  "http://127.0.0.1:" + server.getAddress().getPort() + "/redirected");
          exchange.sendResponseHeaders(307, body.length);
          exchange.getResponseBody().write(body);
          exchange.close();
        });
    server.createContext(
        "/redirected",
        exchange -> {
          redirectedRequests.incrementAndGet();
          exchange.sendResponseHeaders(204, -1);
          exchange.close();
        });
    server.start();
    try {
      PosePeerArmClient client =
          new PosePeerArmClient("http://127.0.0.1:" + server.getAddress().getPort(), token);
      PosePeerArmClient.Response response =
          client.arm(new PosePeerArmClient.Candidate("redirect-test", "leader", 456, 0.9, 0.8));
      check(response.statusCode() == 307, "redirect response is returned to caller");
      check(response.body().equals("redirect refused"), "redirect response body");
      check(
          receivedAuthorization.get().equals("Bearer " + token),
          "credential reaches configured peer");
      check(redirectedRequests.get() == 0, "redirect target must not be contacted");
    } finally {
      server.stop(0);
    }
  }

  private static void expectFailure(Runnable operation, String label) {
    try {
      operation.run();
      throw new AssertionError(label + " did not fail");
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
