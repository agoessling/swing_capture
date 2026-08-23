package com.agoessling.swingcapture;

import com.agoessling.swingcapture.core.coordination.CaptureRole;
import com.agoessling.swingcapture.core.coordination.PairedCoordinationRecord;
import com.agoessling.swingcapture.node.BearerAuthorization;
import com.sun.net.httpserver.HttpServer;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** Loopback transport coverage for station ownership, health polling, and replication routes. */
public final class AutonomousPairPeerClientTest {
  private AutonomousPairPeerClientTest() {}

  public static void main(String[] arguments) throws Exception {
    String token = BearerAuthorization.generate(new SecureRandom());
    HttpServer server =
        HttpServer.create(new InetSocketAddress(InetAddress.getLoopbackAddress(), 0), 0);
    AtomicInteger armAttempts = new AtomicInteger();
    AtomicReference<String> lastAuthorization = new AtomicReference<>();
    AtomicReference<String> lastBody = new AtomicReference<>();
    List<String> authenticatedRequests = Collections.synchronizedList(new ArrayList<>());
    server.createContext(
        "/",
        exchange -> {
          lastAuthorization.set(exchange.getRequestHeaders().getFirst("Authorization"));
          lastBody.set(new String(exchange.getRequestBody().readAllBytes(), StandardCharsets.UTF_8));
          String path = exchange.getRequestURI().getPath();
          String method = exchange.getRequestMethod();
          int status = 200;
          String response = "{}";
          if (!("Bearer " + token).equals(lastAuthorization.get())) {
            status = 401;
            response = "unauthorized";
          } else {
            authenticatedRequests.add(method + " " + path);
            if (path.equals("/api/v1/capture/arm") && armAttempts.incrementAndGet() == 1) {
              status = 503;
              response = "retry";
            } else if (path.equals("/api/v1/capture/trigger-report")) {
              response = "{\"shared_session_id\":\"swing-1\"}";
            } else if (path.equals("/api/v1/sessions/local-1/manifest")
                || path.equals("/api/v1/sessions/missing-media/manifest")) {
              response = "{\"session_id\":\"local-1\"}";
            } else if (path.equals("/api/v1/sessions/missing-manifest/manifest")
                || path.equals("/api/v1/sessions/missing-media/down_the_line.mp4")) {
              status = 404;
            } else if (path.equals("/api/v1/coordination/swing-1")) {
              status = 201;
            }
          }
          byte[] encoded = response.getBytes(StandardCharsets.UTF_8);
          if (method.equals("HEAD")) {
            exchange.sendResponseHeaders(status, -1);
          } else {
            exchange.sendResponseHeaders(status, encoded.length);
            exchange.getResponseBody().write(encoded);
          }
          exchange.close();
        });
    server.start();
    try {
      String origin = "http://127.0.0.1:" + server.getAddress().getPort();
      AutonomousPairPeerClient client = new AutonomousPairPeerClient(origin, token);
      check(client.setStandbyWithRetries(true, 3, 0).accepted(), "503 start recovers");
      check(armAttempts.get() == 2, "start retry count");
      check(lastBody.get().equals("{\"armed\":true}"), "canonical station arm body");
      check(
          authenticatedRequests.contains("POST /api/v1/capture/arm"),
          "peer standby mutation is authenticated");
      check(client.triggerReport().body().contains("swing-1"), "trigger report route");
      check(
          authenticatedRequests.contains("GET /api/v1/capture/trigger-report"),
          "peer trigger report is authenticated");
      AutonomousPairPeerClient.PublicationProbe publication =
          client.publishedCapture("local-1", "down_the_line");
      check(publication.published(), "manifest plus role media prove peer publication");
      check(
          authenticatedRequests.contains("HEAD /api/v1/sessions/local-1/manifest"),
          "bounded publication manifest probe is authenticated");
      check(
          authenticatedRequests.contains("HEAD /api/v1/sessions/local-1/down_the_line.mp4"),
          "bounded publication media probe is authenticated");

      int requestsBeforeMissingManifest = authenticatedRequests.size();
      AutonomousPairPeerClient.PublicationProbe missingManifest =
          client.publishedCapture("missing-manifest", "down_the_line");
      check(!missingManifest.published(), "missing manifest cannot publish a peer clip");
      check(missingManifest.statusCode() == 404, "missing manifest status");
      check(missingManifest.stage().equals("manifest"), "missing manifest stage");
      check(
          authenticatedRequests.size() == requestsBeforeMissingManifest + 1,
          "missing manifest short-circuits the media probe");

      AutonomousPairPeerClient.PublicationProbe missingMedia =
          client.publishedCapture("missing-media", "down_the_line");
      check(!missingMedia.published(), "manifest alone cannot publish a peer clip");
      check(missingMedia.statusCode() == 404, "missing media status");
      check(missingMedia.stage().equals("media"), "missing media stage");
      PairedCoordinationRecord.NodeEvidence downTheLine =
          new PairedCoordinationRecord.NodeEvidence(
              CaptureRole.DOWN_THE_LINE,
              "node-dtl",
              "local-dtl",
              1_000_000,
              1_000,
              1_000_000,
              1_000,
              0,
              0,
              0,
              0,
              1,
              "local_audio");
      PairedCoordinationRecord.NodeEvidence faceOn =
          new PairedCoordinationRecord.NodeEvidence(
              CaptureRole.FACE_ON,
              "node-face",
              "local-face",
              1_002_000,
              1_000,
              1_001_500,
              1_500,
              500,
              500,
              500,
              1_000,
              3,
              "peer_audio_clock_candidate");
      PairedCoordinationRecord record =
          PairedCoordinationRecord.create("swing-1", 1_700_000_000_000L, downTheLine, faceOn);
      check(client.storeCoordinationRecord(record).accepted(), "coordination replication route");
      check(lastBody.get().equals(record.toJson()), "canonical coordination record body");
      check(lastAuthorization.get().equals("Bearer " + token), "bearer credential retained");
      check(
          authenticatedRequests.contains("POST /api/v1/coordination/swing-1"),
          "coordination replication is authenticated");

      String wrongToken = BearerAuthorization.generate(new SecureRandom());
      AutonomousPairPeerClient.Response unauthorized =
          new AutonomousPairPeerClient(origin, wrongToken).setStandbyWithRetries(false, 3, 0);
      check(unauthorized.statusCode() == 401, "wrong credential rejected");
      check(armAttempts.get() == 2, "401 is not retried or admitted");
    } finally {
      server.stop(0);
    }

    new AutonomousPairPeerClient(
        "https://peer:8443",
        token,
        (endpoint, method, authorization, body) ->
            new AutonomousPairPeerClient.Response(200, "{}"));
    expectFailure(() -> new AutonomousPairPeerClient("http://peer:8088/path", token));
    expectFailure(
        () -> {
          try {
            new AutonomousPairPeerClient("http://peer:8088", token)
                .publishedCapture("local-1", "unassigned");
          } catch (java.io.IOException unexpected) {
            throw new AssertionError(unexpected);
          }
        });
  }

  private static void expectFailure(Runnable action) {
    try {
      action.run();
      throw new AssertionError("expected invalid peer origin");
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String label) {
    if (!condition) {
      throw new AssertionError(label);
    }
  }
}
