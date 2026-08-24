package com.agoessling.swingcapture;

import java.io.IOException;
import java.util.List;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** Strict reverse-probe wire codec and additive status serializer. */
final class PairNetworkHealthJson {
  private PairNetworkHealthJson() {}

  static byte[] reverseRequest(String callbackOrigin, String callbackNodeId) throws IOException {
    return PairNetworkHealthWireJson.reverseRequest(callbackOrigin, callbackNodeId);
  }

  static PairNetworkHealthWireJson.ReverseRequest parseReverseRequest(byte[] contents)
      throws IOException {
    return PairNetworkHealthWireJson.parseReverseRequest(contents);
  }

  static byte[] directionBytes(PairNetworkHealthPolicy.DirectionEvidence direction)
      throws IOException {
    return PairNetworkHealthWireJson.directionBytes(direction);
  }

  static PairNetworkHealthPolicy.DirectionEvidence parseDirection(byte[] contents)
      throws IOException {
    return PairNetworkHealthWireJson.parseDirection(contents);
  }

  static JSONObject snapshotJson(PairNetworkHealthPolicy.Snapshot snapshot) throws JSONException {
    JSONObject result =
        new JSONObject()
            .put("schema_version", 1)
            .put("configured", snapshot.configured())
            .put("state", snapshot.state().wireName())
            .put("raw_state", snapshot.rawState().wireName())
            .put("measured", snapshot.measured())
            .put("stale", snapshot.stale())
            .put("transition_pending", snapshot.transitionPending())
            .put("age_ns", Long.toString(snapshot.ageNanos()))
            .put("issues", new JSONArray(snapshot.issues()));
    result.put(
        "peer",
        snapshot.peer() == null
            ? JSONObject.NULL
            : new JSONObject()
                .put("origin", snapshot.peer().origin())
                .put("node_id", snapshot.peer().nodeId()));
    if (snapshot.latestRound() == null) {
      result.put("measured_at_elapsed_realtime_ns", JSONObject.NULL);
      result.put("local_to_peer", JSONObject.NULL);
      result.put("peer_to_local", JSONObject.NULL);
    } else {
      result.put(
          "measured_at_elapsed_realtime_ns",
          Long.toString(snapshot.latestRound().measuredAtElapsedRealtimeNanos()));
      result.put("local_to_peer", directionStatusJson(snapshot.latestRound().localToPeer()));
      result.put("peer_to_local", directionStatusJson(snapshot.latestRound().peerToLocal()));
    }
    return result;
  }

  private static JSONObject directionJson(PairNetworkHealthPolicy.DirectionEvidence direction)
      throws JSONException {
    JSONArray roundTrips = new JSONArray();
    for (long roundTrip : direction.roundTripNanos()) {
      roundTrips.put(Long.toString(roundTrip));
    }
    return new JSONObject()
        .put("schema_version", 1)
        .put("attempts", direction.attempts())
        .put("successes", direction.successes())
        .put("timeouts", direction.timeouts())
        .put("round_trip_ns", roundTrips)
        .put("transfer_bytes", Long.toString(direction.transferBytes()))
        .put("transfer_duration_ns", Long.toString(direction.transferDurationNanos()))
        .put("transfer_complete", direction.transferComplete());
  }

  private static JSONObject directionStatusJson(
      PairNetworkHealthPolicy.DirectionEvidence direction) throws JSONException {
    return directionJson(direction)
        .put("minimum_round_trip_ns", Long.toString(direction.minimumRoundTripNanos()))
        .put("median_round_trip_ns", Long.toString(direction.medianRoundTripNanos()))
        .put("p95_round_trip_ns", Long.toString(direction.p95RoundTripNanos()))
        .put("maximum_round_trip_ns", Long.toString(direction.maximumRoundTripNanos()))
        .put("jitter_ns", Long.toString(direction.jitterNanos()))
        .put("transfer_bits_per_second", direction.transferBitsPerSecond());
  }

}
