package com.agoessling.swingcapture;

/** Exact mapped, legacy fallback, and invalid selection coverage for retained field evidence. */
public final class PeerImpactMappingManifestTest {
  private PeerImpactMappingManifestTest() {}

  public static void main(String[] arguments) {
    serializesMappedClockCandidate();
    serializesLegacyArrivalFallback();
    preservesRejectedMappingAndEffectiveFallback();
    rejectsInvalidSelectionEvidence();
    rejectsIncompatibleSourceAndRequest();
  }

  private static void serializesMappedClockCandidate() {
    PosePeerArmClient.ImpactTrigger request =
        PosePeerArmClient.ImpactTrigger.mapped(
            "shared-1", "leader-1", 1_000, "shadow-1", 2_000, 20, 30, 40, 50, 3);
    String actual =
        PeerImpactMappingManifest.create(
                request, request, "peer_audio_clock_candidate", 2_100, 2_005, 7)
            .toCanonicalJson();
    String expected =
        "{\"schema_version\":2,\"request_schema_version\":2,"
            + "\"mapping_policy\":\"accepted\",\"effective_request_schema_version\":2,"
            + "\"fallback_semantics\":\"mapped_candidate_else_arrival\","
            + "\"leader_node_id\":\"leader-1\",\"leader_trigger_elapsed_realtime_ns\":\"1000\","
            + "\"target_peer_node_id\":\"shadow-1\","
            + "\"mapped_peer_trigger_elapsed_realtime_ns\":\"2000\","
            + "\"mapping_uncertainty_ns\":\"20\",\"mapping_age_at_send_ns\":\"30\","
            + "\"minimum_round_trip_ns\":\"40\",\"maximum_round_trip_ns\":\"50\","
            + "\"clock_sample_count\":3,\"request_arrival_elapsed_realtime_ns\":\"2100\","
            + "\"selection_source\":\"peer_audio_clock_candidate\","
            + "\"selected_local_trigger_elapsed_realtime_ns\":\"2005\","
            + "\"selected_local_uncertainty_ns\":\"7\","
            + "\"selected_to_mapped_residual_ns\":\"5\"}";
    check(actual.equals(expected), "mapped evidence JSON: " + actual);
  }

  private static void serializesLegacyArrivalFallback() {
    PosePeerArmClient.ImpactTrigger request =
        new PosePeerArmClient.ImpactTrigger("shared-2", "leader-2", 5_000);
    String actual =
        PeerImpactMappingManifest.create(
                request, request, "peer_audio_arrival", 6_000, 6_000, 0)
            .toCanonicalJson();
    check(actual.contains("\"request_schema_version\":1"), "legacy request schema");
    check(actual.contains("\"mapping_policy\":\"not_applicable\""), "legacy policy");
    check(actual.contains("\"mapping_uncertainty_ns\":null"), "mapping is explicitly absent");
    check(actual.contains("\"selected_to_mapped_residual_ns\":null"), "residual is absent");
  }

  private static void preservesRejectedMappingAndEffectiveFallback() {
    PosePeerArmClient.ImpactTrigger original =
        PosePeerArmClient.ImpactTrigger.mapped(
            "shared-rejected", "leader-rejected", 10_000, "shadow-rejected", 20_000,
            30_000_000, 11_000_000_000L, 40, 50, 3);
    String actual =
        PeerImpactMappingManifest.create(
                original,
                original.withoutClockMapping(),
                "peer_audio_local_candidate",
                20_200,
                20_005,
                7)
            .toCanonicalJson();
    check(actual.contains("\"request_schema_version\":2"), "original schema retained");
    check(actual.contains("\"mapping_policy\":\"rejected\""), "rejection retained");
    check(
        actual.contains("\"effective_request_schema_version\":1"),
        "effective fallback schema retained");
    check(
        actual.contains("\"fallback_semantics\":\"fresh_local_candidate_else_arrival\""),
        "effective fallback semantics retained");
    check(actual.contains("\"mapping_uncertainty_ns\":\"30000000\""), "uncertainty retained");
    check(actual.contains("\"mapping_age_at_send_ns\":\"11000000000\""), "age retained");
    check(actual.contains("\"selected_to_mapped_residual_ns\":\"5\""), "residual retained");
  }

  private static void rejectsInvalidSelectionEvidence() {
    PosePeerArmClient.ImpactTrigger request =
        new PosePeerArmClient.ImpactTrigger("shared-3", "leader-3", 7_000);
    expectFailure(
        () -> PeerImpactMappingManifest.create(request, request, "unknown", 8_000, 8_000, 0));
    expectFailure(
        () ->
            PeerImpactMappingManifest.create(
                request, request, "peer_audio_arrival", 8_000, 8_000, -1));
  }

  private static void rejectsIncompatibleSourceAndRequest() {
    PosePeerArmClient.ImpactTrigger legacy =
        new PosePeerArmClient.ImpactTrigger("shared-4", "leader-4", 9_000);
    PosePeerArmClient.ImpactTrigger mapped =
        PosePeerArmClient.ImpactTrigger.mapped(
            "shared-5", "leader-5", 10_000, "shadow-5", 20_000, 20, 30, 40, 50, 3);
    expectFailure(
        () ->
            PeerImpactMappingManifest.create(
                legacy, legacy, "peer_audio_clock_candidate", 9_100, 9_050, 5));
    expectFailure(
        () ->
            PeerImpactMappingManifest.create(
                mapped, mapped, "peer_audio_local_candidate", 20_100, 20_050, 5));
    expectFailure(
        () ->
            PeerImpactMappingManifest.create(
                mapped,
                new PosePeerArmClient.ImpactTrigger("different", "leader-5", 10_000),
                "peer_audio_arrival",
                20_100,
                20_100,
                0));
  }

  private static void expectFailure(Runnable operation) {
    try {
      operation.run();
      throw new AssertionError("expected IllegalArgumentException");
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
