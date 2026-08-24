package com.agoessling.swingcapture;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.List;

public final class PairNetworkHealthWireJsonTest {
  private PairNetworkHealthWireJsonTest() {}

  public static void main(String[] arguments) throws Exception {
    reverseRequestRoundTripsEscapedStrings();
    directionRoundTripsExactlyAndAllowsFieldReordering();
    strictRequestSchemaRejectsAmbiguity();
    strictDirectionSchemaRejectsAmbiguity();
    wireBoundsAndUtf8AreEnforced();
  }

  private static void reverseRequestRoundTripsEscapedStrings() throws Exception {
    String origin = "http://192.168.4.20:8088";
    String nodeId = "node-\"quoted\"-\\-\n";
    PairNetworkHealthWireJson.ReverseRequest parsed =
        PairNetworkHealthWireJson.parseReverseRequest(
            PairNetworkHealthWireJson.reverseRequest(origin, nodeId));
    check(parsed.callbackOrigin().equals(origin), "callback origin round trip");
    check(parsed.callbackNodeId().equals(nodeId), "escaped callback node ID round trip");
  }

  private static void directionRoundTripsExactlyAndAllowsFieldReordering() throws Exception {
    PairNetworkHealthPolicy.DirectionEvidence expected =
        new PairNetworkHealthPolicy.DirectionEvidence(
            3, 2, 1, List.of(30L, 10L), 1_048_576, 123_456_789, true);
    PairNetworkHealthPolicy.DirectionEvidence parsed =
        PairNetworkHealthWireJson.parseDirection(
            PairNetworkHealthWireJson.directionBytes(expected));
    check(parsed.equals(expected), "directed evidence strict round trip");

    String reordered =
        "{\"transfer_complete\":true,\"transfer_duration_ns\":\"123456789\","
            + "\"transfer_bytes\":\"1048576\",\"round_trip_ns\":[\"10\",\"30\"],"
            + "\"timeouts\":1,\"successes\":2,\"attempts\":3,\"schema_version\":1}";
    check(
        PairNetworkHealthWireJson.parseDirection(utf8(reordered)).equals(expected),
        "JSON object field order is immaterial");
  }

  private static void strictRequestSchemaRejectsAmbiguity() {
    for (String invalid :
        new String[] {
          "{}",
          "{\"schema_version\":1,\"callback_origin\":\"http://192.168.4.20:8088\","
              + "\"callback_node_id\":\"peer\",\"extra\":true}",
          "{\"schema_version\":1,\"schema_version\":1,"
              + "\"callback_origin\":\"http://192.168.4.20:8088\","
              + "\"callback_node_id\":\"peer\"}",
          "{\"schema_version\":1.0,\"callback_origin\":\"http://192.168.4.20:8088\","
              + "\"callback_node_id\":\"peer\"}",
          "{\"schema_version\":1,\"callback_origin\":\"http://192.168.4.20:8088\","
              + "\"callback_node_id\":\"peer\"} trailing",
          "{\"schema_version\":1,\"callback_origin\":\"\","
              + "\"callback_node_id\":\"peer\"}"
        }) {
      expectIOException(() -> PairNetworkHealthWireJson.parseReverseRequest(utf8(invalid)));
    }
  }

  private static void strictDirectionSchemaRejectsAmbiguity() {
    String prefix =
        "{\"schema_version\":1,\"attempts\":3,\"successes\":2,\"timeouts\":1,"
            + "\"round_trip_ns\":[\"10\",\"30\"],\"transfer_bytes\":\"1048576\","
            + "\"transfer_duration_ns\":\"123456789\",\"transfer_complete\":";
    expectIOException(() -> PairNetworkHealthWireJson.parseDirection(utf8(prefix + "1}")));
    expectIOException(
        () ->
            PairNetworkHealthWireJson.parseDirection(
                utf8(prefix.replace("\"1048576\"", "\"01048576\"") + "true}")));
    expectIOException(
        () ->
            PairNetworkHealthWireJson.parseDirection(
                utf8(prefix.replace("[\"10\",\"30\"]", "[10,\"30\"]") + "true}")));
    expectIOException(
        () ->
            PairNetworkHealthWireJson.parseDirection(
                utf8(prefix.replace("\"successes\":2", "\"successes\":3") + "true}")));
  }

  private static void wireBoundsAndUtf8AreEnforced() {
    byte[] oversized = new byte[PairNetworkHealthWireJson.MAXIMUM_REVERSE_REQUEST_BYTES + 1];
    expectIOException(() -> PairNetworkHealthWireJson.parseReverseRequest(oversized));
    expectIOException(
        () -> PairNetworkHealthWireJson.parseReverseRequest(new byte[] {(byte) 0xc3, 0x28}));
    expectIOException(
        () ->
            PairNetworkHealthWireJson.reverseRequest(
                "http://192.168.4.20:8088",
                "x".repeat(PairNetworkHealthWireJson.MAXIMUM_REVERSE_REQUEST_BYTES)));
  }

  private static byte[] utf8(String value) {
    return value.getBytes(StandardCharsets.UTF_8);
  }

  private static void expectIOException(ThrowingAction action) {
    try {
      action.run();
      throw new AssertionError("malformed JSON was accepted");
    } catch (IOException expected) {
      // Expected.
    } catch (Exception unexpected) {
      throw new AssertionError("expected IOException", unexpected);
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
