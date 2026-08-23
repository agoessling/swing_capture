package com.agoessling.swingcapture;

import java.util.Set;

/** Immutable field evidence for one peer impact request and the local timestamp selected for it. */
final class PeerImpactMappingManifest {
  static final int SCHEMA_VERSION = 2;
  private static final Set<String> SELECTION_SOURCES =
      Set.of(
          "peer_audio_clock_candidate",
          "peer_audio_local_candidate",
          "peer_audio_arrival");

  private final PosePeerArmClient.ImpactTrigger originalRequest;
  private final PosePeerArmClient.ImpactTrigger effectiveRequest;
  private final String mappingPolicy;
  private final String fallbackSemantics;
  private final String selectionSource;
  private final long requestArrivalElapsedRealtimeNanos;
  private final long selectedLocalTriggerElapsedRealtimeNanos;
  private final long selectedLocalUncertaintyNanos;

  static PeerImpactMappingManifest create(
      PosePeerArmClient.ImpactTrigger originalRequest,
      PosePeerArmClient.ImpactTrigger effectiveRequest,
      String selectionSource,
      long requestArrivalElapsedRealtimeNanos,
      long selectedLocalTriggerElapsedRealtimeNanos,
      long selectedLocalUncertaintyNanos) {
    return new PeerImpactMappingManifest(
        originalRequest,
        effectiveRequest,
        selectionSource,
        requestArrivalElapsedRealtimeNanos,
        selectedLocalTriggerElapsedRealtimeNanos,
        selectedLocalUncertaintyNanos);
  }

  private PeerImpactMappingManifest(
      PosePeerArmClient.ImpactTrigger originalRequest,
      PosePeerArmClient.ImpactTrigger effectiveRequest,
      String selectionSource,
      long requestArrivalElapsedRealtimeNanos,
      long selectedLocalTriggerElapsedRealtimeNanos,
      long selectedLocalUncertaintyNanos) {
    this.originalRequest = java.util.Objects.requireNonNull(originalRequest, "originalRequest");
    this.effectiveRequest = java.util.Objects.requireNonNull(effectiveRequest, "effectiveRequest");
    this.selectionSource = java.util.Objects.requireNonNull(selectionSource, "selectionSource");
    requireCompatibleDecision(originalRequest, effectiveRequest, selectionSource);
    if (!originalRequest.hasClockMapping()) {
      mappingPolicy = "not_applicable";
    } else if (effectiveRequest.hasClockMapping()) {
      mappingPolicy = "accepted";
    } else {
      mappingPolicy = "rejected";
    }
    fallbackSemantics =
        effectiveRequest.hasClockMapping()
            ? "mapped_candidate_else_arrival"
            : "fresh_local_candidate_else_arrival";
    if (!SELECTION_SOURCES.contains(selectionSource)
        || requestArrivalElapsedRealtimeNanos <= 0
        || selectedLocalTriggerElapsedRealtimeNanos <= 0
        || selectedLocalUncertaintyNanos < 0) {
      throw new IllegalArgumentException("peer impact mapping selection evidence is invalid");
    }
    this.requestArrivalElapsedRealtimeNanos = requestArrivalElapsedRealtimeNanos;
    this.selectedLocalTriggerElapsedRealtimeNanos = selectedLocalTriggerElapsedRealtimeNanos;
    this.selectedLocalUncertaintyNanos = selectedLocalUncertaintyNanos;
  }

  String toCanonicalJson() {
    StringBuilder json = new StringBuilder(768);
    json.append('{');
    appendNumber(json, "schema_version", SCHEMA_VERSION);
    appendNumber(json, "request_schema_version", originalRequest.schemaVersion());
    appendString(json, "mapping_policy", mappingPolicy);
    appendNumber(json, "effective_request_schema_version", effectiveRequest.schemaVersion());
    appendString(json, "fallback_semantics", fallbackSemantics);
    appendString(json, "leader_node_id", originalRequest.leaderNodeId());
    appendDecimal(
        json,
        "leader_trigger_elapsed_realtime_ns",
        originalRequest.leaderTriggerElapsedRealtimeNanos());
    if (originalRequest.hasClockMapping()) {
      appendString(json, "target_peer_node_id", originalRequest.targetPeerNodeId());
      appendDecimal(
          json,
          "mapped_peer_trigger_elapsed_realtime_ns",
          originalRequest.mappedPeerTriggerElapsedRealtimeNanos());
      appendDecimal(json, "mapping_uncertainty_ns", originalRequest.mappingUncertaintyNanos());
      appendDecimal(json, "mapping_age_at_send_ns", originalRequest.mappingAgeAtSendNanos());
      appendDecimal(json, "minimum_round_trip_ns", originalRequest.minimumRoundTripNanos());
      appendDecimal(json, "maximum_round_trip_ns", originalRequest.maximumRoundTripNanos());
      appendNumber(json, "clock_sample_count", originalRequest.sampleCount());
    } else {
      appendNull(json, "target_peer_node_id");
      appendNull(json, "mapped_peer_trigger_elapsed_realtime_ns");
      appendNull(json, "mapping_uncertainty_ns");
      appendNull(json, "mapping_age_at_send_ns");
      appendNull(json, "minimum_round_trip_ns");
      appendNull(json, "maximum_round_trip_ns");
      appendNumber(json, "clock_sample_count", 0);
    }
    appendDecimal(json, "request_arrival_elapsed_realtime_ns", requestArrivalElapsedRealtimeNanos);
    appendString(json, "selection_source", selectionSource);
    appendDecimal(
        json,
        "selected_local_trigger_elapsed_realtime_ns",
        selectedLocalTriggerElapsedRealtimeNanos);
    appendDecimal(json, "selected_local_uncertainty_ns", selectedLocalUncertaintyNanos);
    if (originalRequest.hasClockMapping()) {
      appendDecimal(
          json,
          "selected_to_mapped_residual_ns",
          absoluteDifference(
              selectedLocalTriggerElapsedRealtimeNanos,
              originalRequest.mappedPeerTriggerElapsedRealtimeNanos()));
    } else {
      appendNull(json, "selected_to_mapped_residual_ns");
    }
    return json.append('}').toString();
  }

  private static void requireCompatibleDecision(
      PosePeerArmClient.ImpactTrigger originalRequest,
      PosePeerArmClient.ImpactTrigger effectiveRequest,
      String selectionSource) {
    PosePeerArmClient.ImpactTrigger expectedFallback = originalRequest.withoutClockMapping();
    if ((!effectiveRequest.equals(originalRequest) && !effectiveRequest.equals(expectedFallback))
        || (originalRequest.schemaVersion() == 1 && !effectiveRequest.equals(originalRequest))) {
      throw new IllegalArgumentException(
          "effective peer impact request is unrelated to the original");
    }
    if (("peer_audio_clock_candidate".equals(selectionSource)
            && !effectiveRequest.hasClockMapping())
        || ("peer_audio_local_candidate".equals(selectionSource)
            && effectiveRequest.hasClockMapping())) {
      throw new IllegalArgumentException(
          "peer impact selection source is incompatible with the effective request");
    }
  }

  private static long absoluteDifference(long first, long second) {
    return first >= second ? first - second : second - first;
  }

  private static void appendNumber(StringBuilder json, String name, long value) {
    appendName(json, name);
    json.append(value);
  }

  private static void appendDecimal(StringBuilder json, String name, long value) {
    appendName(json, name);
    json.append('"').append(value).append('"');
  }

  private static void appendString(StringBuilder json, String name, String value) {
    appendName(json, name);
    json.append('"').append(value).append('"');
  }

  private static void appendNull(StringBuilder json, String name) {
    appendName(json, name);
    json.append("null");
  }

  private static void appendName(StringBuilder json, String name) {
    if (json.charAt(json.length() - 1) != '{') {
      json.append(',');
    }
    json.append('"').append(name).append("\":");
  }
}
