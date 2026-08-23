package com.agoessling.swingcapture;

import com.sun.net.httpserver.HttpServer;
import com.agoessling.swingcapture.node.BearerAuthorization;
import com.agoessling.swingcapture.node.NodeCoordinationState;
import java.io.IOException;
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
    AtomicReference<String> impactRequest = new AtomicReference<>();
    PosePeerArmClient client =
        new PosePeerArmClient(
            "http://192.168.1.20:8088",
            token,
            (endpoint, authorization, body) -> {
              check(endpoint.getHost().equals("192.168.1.20"), "endpoint host");
              check(authorization.equals("Bearer " + token), "authorization");
              if (endpoint.getPath().equals("/api/v1/capture/pose-arm")) {
                request.set(new String(body, StandardCharsets.UTF_8));
              } else if (endpoint.getPath().equals("/api/v1/capture/pose-impact")) {
                impactRequest.set(new String(body, StandardCharsets.UTF_8));
              } else {
                throw new AssertionError("unexpected endpoint " + endpoint);
              }
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
    PosePeerArmClient.Response impactResponse =
        client.triggerImpact(new PosePeerArmClient.ImpactTrigger("pose-1", "leader-node", 789));
    check(impactResponse.accepted(), "accepted impact response");
    check(
        impactRequest
            .get()
            .equals(
                "{\"schema_version\":1,\"shared_session_id\":\"pose-1\","
                    + "\"leader_node_id\":\"leader-node\","
                    + "\"leader_trigger_elapsed_realtime_ns\":\"789\"}"),
        "canonical impact request");
    PosePeerArmClient.ImpactTrigger mapped =
        PosePeerArmClient.ImpactTrigger.mapped(
            "pose-2", "leader-node", 900, "shadow-node", 1_100, 3, 4, 5, 9, 3);
    client.triggerImpact(mapped);
    check(mapped.hasClockMapping(), "mapped trigger schema");
    check(
        impactRequest
            .get()
            .equals(
                "{\"schema_version\":2,\"shared_session_id\":\"pose-2\","
                    + "\"leader_node_id\":\"leader-node\","
                    + "\"leader_trigger_elapsed_realtime_ns\":\"900\","
                    + "\"target_peer_node_id\":\"shadow-node\","
                    + "\"mapped_peer_trigger_elapsed_realtime_ns\":\"1100\","
                    + "\"mapping_uncertainty_ns\":\"3\","
                    + "\"mapping_age_at_send_ns\":\"4\","
                    + "\"minimum_round_trip_ns\":\"5\","
                    + "\"maximum_round_trip_ns\":\"9\",\"sample_count\":3}"),
        "canonical mapped impact request");
    check(
        client.impactEndpoint().toString().equals("http://192.168.1.20:8088/api/v1/capture/pose-impact"),
        "impact endpoint");

    NodeCoordinationState coordination = new NodeCoordinationState();
    coordination.armed("pose-shared");
    coordination.triggered("face_on", "leader-node", "android-local", 789, 3, "local_audio");
    check(
        PosePeerArmClient.sharedSessionIdForLocalTrigger(
                coordination.latestTrigger(), "android-local")
            .equals("pose-shared"),
        "peer impact must carry the coordinated shared session, not the local clip id");
    expectFailure(
        () ->
            PosePeerArmClient.sharedSessionIdForLocalTrigger(
                coordination.latestTrigger(), "another-local-clip"),
        "mismatched local trigger report");

    PosePeerArmClient protectedClient =
        new PosePeerArmClient(
            "https://host:8443",
            token,
            (endpoint, authorization, body) -> new PosePeerArmClient.Response(202, "{}"));
    check(
        protectedClient.endpoint().toString().equals("https://host:8443/api/v1/capture/pose-arm"),
        "HTTPS peer endpoint");
    expectFailure(() -> new PosePeerArmClient("http://host:8088/path", token), "origin path");
    expectFailure(() -> new PosePeerArmClient("http://host", token), "missing port");
    expectFailure(() -> new PosePeerArmClient("http://host:8088", "bad"), "bad token");
    expectFailure(
        () -> new PosePeerArmClient.Candidate("pose-1", "leader", 0, 0.9, 0.8),
        "zero timestamp");
    expectFailure(
        () -> new PosePeerArmClient.Candidate("pose-1", "leader", 1, 1.1, 0.8),
        "invalid confidence");
    expectFailure(
        () -> new PosePeerArmClient.ImpactTrigger("pose-1", "leader", 0),
        "zero impact timestamp");
    expectFailure(
        () ->
            PosePeerArmClient.ImpactTrigger.mapped(
                "pose-1", "leader", 1, "shadow", 2, 0, 0, 0, 0, 2),
        "mapping requires complete estimate batch");
    expectFailure(
        () ->
            new PosePeerArmClient.ImpactTrigger(
                1, "pose-1", "leader", 1, "shadow", 2, 0, 0, 0, 0, 3),
        "schema 1 rejects mapping fields");
    check(!new PosePeerArmClient.Response(409, "busy").accepted(), "rejected response");
    retriesTransientImpactDelivery(token);
    retriesTransientArmDelivery(token);
    waitsForPeerPreRollReadiness(token);
    boundsPeerPreRollReadinessPolling(token);
    cancelsReadinessPollingWhenTheArmLifecycleEnds(token);
    retriesAuthenticatedArmAcrossHttp(token);
    doesNotRetryImpactClientFailure(token);
    doesNotRetryArmAuthenticationFailure(token);
    fallsBackAfterMappedImpactRejection(token);
    rejectsRedirectsWithoutForwardingCredential(token);
  }

  private static void retriesTransientImpactDelivery(String token) throws Exception {
    AtomicInteger attempts = new AtomicInteger();
    PosePeerArmClient client =
        new PosePeerArmClient(
            "http://peer.local:8088",
            token,
            (endpoint, authorization, body) -> {
              int attempt = attempts.incrementAndGet();
              if (attempt == 1) {
                throw new IOException("temporary route failure");
              }
              return new PosePeerArmClient.Response(attempt == 2 ? 503 : 202, "{}");
            });

    PosePeerArmClient.Response response =
        client.triggerImpactWithRetries(
            new PosePeerArmClient.ImpactTrigger("session-retry", "leader", 1), 3, 0);

    check(response.accepted(), "third transient delivery attempt should succeed");
    check(attempts.get() == 3, "transient delivery attempt count");
  }

  private static void retriesTransientArmDelivery(String token) throws Exception {
    AtomicInteger attempts = new AtomicInteger();
    PosePeerArmClient client =
        new PosePeerArmClient(
            "http://peer.local:8088",
            token,
            (endpoint, authorization, body) -> {
              int attempt = attempts.incrementAndGet();
              check(endpoint.getPath().equals("/api/v1/capture/pose-arm"), "arm retry path");
              check(authorization.equals("Bearer " + token), "arm retry authorization");
              if (attempt == 1) {
                throw new IOException("temporary route failure");
              }
              return new PosePeerArmClient.Response(attempt == 2 ? 503 : 202, "{}");
            });

    PosePeerArmClient.Response response =
        client.armWithRetries(
            new PosePeerArmClient.Candidate("session-arm-retry", "leader", 1, 0.9, 0.8),
            3,
            0);

    check(response.accepted(), "third transient arm delivery attempt should succeed");
    check(attempts.get() == 3, "transient arm delivery attempt count");
  }

  private static void waitsForPeerPreRollReadiness(String token) throws Exception {
    AtomicInteger attempts = new AtomicInteger();
    PosePeerArmClient client =
        new PosePeerArmClient(
            "http://peer.local:8088",
            token,
            (endpoint, authorization, body) -> {
              int attempt = attempts.incrementAndGet();
              return new PosePeerArmClient.Response(202, "{}", attempt == 3);
            });

    PosePeerArmClient.Response response =
        client.armUntilReady(
            new PosePeerArmClient.Candidate("session-ready", "leader", 1, 0.9, 0.8),
            2,
            4,
            0);

    check(response.poseReady(), "third admitted arm response confirms peer pre-roll");
    check(attempts.get() == 3, "pending 202 responses are retried until readiness");
  }

  private static void boundsPeerPreRollReadinessPolling(String token) throws Exception {
    AtomicInteger attempts = new AtomicInteger();
    PosePeerArmClient client =
        new PosePeerArmClient(
            "http://peer.local:8088",
            token,
            (endpoint, authorization, body) -> {
              attempts.incrementAndGet();
              return new PosePeerArmClient.Response(202, "{}", false);
            });

    PosePeerArmClient.Response response =
        client.armUntilReady(
            new PosePeerArmClient.Candidate("session-pending", "leader", 1, 0.9, 0.8),
            2,
            3,
            0);

    check(response.accepted() && !response.poseReady(), "readiness timeout remains unconfirmed");
    check(attempts.get() == 3, "readiness polling honors its independent bound");
  }

  private static void cancelsReadinessPollingWhenTheArmLifecycleEnds(String token)
      throws Exception {
    AtomicInteger attempts = new AtomicInteger();
    PosePeerArmClient client =
        new PosePeerArmClient(
            "http://peer.local:8088",
            token,
            (endpoint, authorization, body) -> {
              attempts.incrementAndGet();
              return new PosePeerArmClient.Response(202, "{}", true);
            });

    try {
      client.armUntilReady(
          new PosePeerArmClient.Candidate("session-cancelled", "leader", 1, 0.9, 0.8),
          2,
          10,
          0,
          () -> attempts.get() == 0);
      throw new AssertionError("ended arm lifecycle must cancel readiness polling");
    } catch (PosePeerArmClient.ArmPollingCancelledException expected) {
      check(attempts.get() == 1, "cancellation stops before another peer request");
    }
  }

  private static void retriesAuthenticatedArmAcrossHttp(String token) throws Exception {
    HttpServer server =
        HttpServer.create(new InetSocketAddress(InetAddress.getLoopbackAddress(), 0), 0);
    AtomicInteger validAttempts = new AtomicInteger();
    AtomicInteger unauthorizedAttempts = new AtomicInteger();
    AtomicReference<String> receivedBody = new AtomicReference<>();
    server.createContext(
        "/api/v1/capture/pose-arm",
        exchange -> {
          byte[] requestBody = exchange.getRequestBody().readAllBytes();
          receivedBody.set(new String(requestBody, StandardCharsets.UTF_8));
          boolean authorized =
              ("Bearer " + token)
                  .equals(exchange.getRequestHeaders().getFirst("Authorization"));
          int status;
          String response;
          if (!authorized) {
            unauthorizedAttempts.incrementAndGet();
            status = 401;
            response = "unauthorized";
          } else if (validAttempts.incrementAndGet() == 1) {
            status = 503;
            response = "temporarily unavailable";
          } else {
            status = 202;
            response = "{}";
            exchange.getResponseHeaders().set(PosePeerArmClient.POSE_READY_HEADER, "true");
          }
          byte[] responseBody = response.getBytes(StandardCharsets.UTF_8);
          exchange.sendResponseHeaders(status, responseBody.length);
          exchange.getResponseBody().write(responseBody);
          exchange.close();
        });
    server.start();
    try {
      String origin = "http://127.0.0.1:" + server.getAddress().getPort();
      PosePeerArmClient.Candidate candidate =
          new PosePeerArmClient.Candidate("session-http-retry", "leader", 42, 0.9, 0.8);
      PosePeerArmClient.Response response =
          new PosePeerArmClient(origin, token).armWithRetries(candidate, 3, 0);
      check(response.accepted(), "authenticated HTTP arm retry should recover from 503");
      check(response.poseReady(), "HTTP transport preserves the peer readiness header");
      check(validAttempts.get() == 2, "HTTP arm retry count");
      check(
          receivedBody.get().equals(new String(candidate.requestBody(), StandardCharsets.UTF_8)),
          "HTTP arm request body");

      String differentToken = BearerAuthorization.generate(new SecureRandom());
      PosePeerArmClient.Response unauthorized =
          new PosePeerArmClient(origin, differentToken).armWithRetries(candidate, 3, 0);
      check(unauthorized.statusCode() == 401, "HTTP server rejects the wrong peer token");
      check(unauthorizedAttempts.get() == 1, "401 response is not retried over HTTP");
    } finally {
      server.stop(0);
    }
  }

  private static void doesNotRetryImpactClientFailure(String token) throws Exception {
    AtomicInteger attempts = new AtomicInteger();
    PosePeerArmClient client =
        new PosePeerArmClient(
            "http://peer.local:8088",
            token,
            (endpoint, authorization, body) -> {
              attempts.incrementAndGet();
              return new PosePeerArmClient.Response(401, "unauthorized");
            });

    PosePeerArmClient.Response response =
        client.triggerImpactWithRetries(
            new PosePeerArmClient.ImpactTrigger("session-auth", "leader", 1), 3, 0);

    check(response.statusCode() == 401, "client failure should be returned");
    check(attempts.get() == 1, "client failure must not be retried");
  }

  private static void doesNotRetryArmAuthenticationFailure(String token) throws Exception {
    AtomicInteger attempts = new AtomicInteger();
    PosePeerArmClient client =
        new PosePeerArmClient(
            "http://peer.local:8088",
            token,
            (endpoint, authorization, body) -> {
              attempts.incrementAndGet();
              return new PosePeerArmClient.Response(401, "unauthorized");
            });

    PosePeerArmClient.Response response =
        client.armWithRetries(
            new PosePeerArmClient.Candidate("session-arm-auth", "leader", 1, 0.9, 0.8),
            3,
            0);

    check(response.statusCode() == 401, "arm authentication failure should be returned");
    check(attempts.get() == 1, "arm authentication failure must not be retried");
  }

  private static void fallsBackAfterMappedImpactRejection(String token) throws Exception {
    AtomicInteger attempts = new AtomicInteger();
    AtomicReference<String> fallbackBody = new AtomicReference<>();
    PosePeerArmClient client =
        new PosePeerArmClient(
            "http://peer.local:8088",
            token,
            (endpoint, authorization, body) -> {
              int attempt = attempts.incrementAndGet();
              String requestBody = new String(body, StandardCharsets.UTF_8);
              if (attempt == 1) {
                check(requestBody.contains("\"schema_version\":2"), "first mapped attempt");
                return new PosePeerArmClient.Response(400, "mapping targets another node");
              }
              fallbackBody.set(requestBody);
              return new PosePeerArmClient.Response(202, "{}");
            });

    PosePeerArmClient.Response response =
        client.triggerImpactWithRetriesAndMappingFallback(
            PosePeerArmClient.ImpactTrigger.mapped(
                "session-fallback", "leader", 100, "old-peer", 200, 3, 4, 5, 9, 3),
            3,
            0);

    check(response.accepted(), "schema-1 fallback should be accepted");
    check(attempts.get() == 2, "one mapped attempt and one fallback attempt");
    check(
        fallbackBody.get().equals(
            "{\"schema_version\":1,\"shared_session_id\":\"session-fallback\","
                + "\"leader_node_id\":\"leader\","
                + "\"leader_trigger_elapsed_realtime_ns\":\"100\"}"),
        "fallback body drops stale mapping fields");
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
