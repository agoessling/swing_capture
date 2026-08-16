package com.agoessling.swingcapture;

import com.agoessling.swingcapture.core.coordination.AtomicTextStore;
import com.agoessling.swingcapture.core.coordination.CaptureRole;
import com.agoessling.swingcapture.core.coordination.CoordinationRecordStore;
import com.agoessling.swingcapture.core.coordination.PairedCoordinationRecord;
import com.agoessling.swingcapture.core.coordination.PairedCoordinationRecord.NodeEvidence;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;
import java.util.Optional;

/** Hermetic route-domain tests for exact GET/POST coordination semantics. */
public final class CoordinationHttpEndpointTest {
  private static final String SHARED_SESSION = "shared-42";

  private CoordinationHttpEndpointTest() {}

  public static void main(String[] arguments) throws Exception {
    missingAndInvalidReadsAreExplicit();
    postIsStrictIdempotentAndImmutable();
    malformedMismatchedAndUnboundedBodiesAreRejected();
  }

  private static void missingAndInvalidReadsAreExplicit() throws Exception {
    CoordinationHttpEndpoint endpoint = endpoint();
    checkResponse(endpoint.get(SHARED_SESSION), 404, 0, "missing");
    checkResponse(endpoint.get("bad/session"), 400, 0, "invalid");
  }

  private static void postIsStrictIdempotentAndImmutable() throws Exception {
    CoordinationHttpEndpoint endpoint = endpoint();
    PairedCoordinationRecord first = record(1_700_000_000_000L);
    byte[] firstBody = first.toJson().getBytes(StandardCharsets.UTF_8);

    CoordinationHttpEndpoint.Response stored = endpoint.post(SHARED_SESSION, firstBody);
    checkResponse(stored, 201, 1, "stored");
    check(stored.body().equals(first.toJson()), "created response is canonical evidence");

    CoordinationHttpEndpoint.Response present = endpoint.get(SHARED_SESSION);
    checkResponse(present, 200, 1, "present");
    check(present.body().equals(first.toJson()), "GET returns canonical evidence");

    CoordinationHttpEndpoint.Response replay = endpoint.post(SHARED_SESSION, firstBody);
    checkResponse(replay, 200, 1, "already_present");
    check(replay.body().equals(first.toJson()), "idempotent response is stored evidence");

    CoordinationHttpEndpoint.Response conflict =
        endpoint.post(
            SHARED_SESSION,
            record(1_700_000_000_001L).toJson().getBytes(StandardCharsets.UTF_8));
    checkResponse(conflict, 409, 1, "conflict");
    check(conflict.body().equals(first.toJson()), "conflict returns authoritative winner");
    check(endpoint.get(SHARED_SESSION).body().equals(first.toJson()), "conflict cannot overwrite");
  }

  private static void malformedMismatchedAndUnboundedBodiesAreRejected() throws Exception {
    CoordinationHttpEndpoint endpoint = endpoint();
    checkResponse(endpoint.post(SHARED_SESSION, new byte[0]), 400, 0, "invalid");
    checkResponse(
        endpoint.post(SHARED_SESSION, new byte[] {(byte) 0xc3, (byte) 0x28}),
        400,
        0,
        "invalid");
    checkResponse(
        endpoint.post(
            SHARED_SESSION,
            "{}".getBytes(StandardCharsets.UTF_8)),
        400,
        0,
        "invalid");
    String overflowingTiming =
        record(1_700_000_000_000L)
            .toJson()
            .replace(
                "\"trigger_timestamp_ns\":\"10000000\"",
                "\"trigger_timestamp_ns\":\"9223372036854775807\"")
            .replace("\"clock_offset_ns\":\"1000000\"", "\"clock_offset_ns\":\"-1\"");
    checkResponse(
        endpoint.post(SHARED_SESSION, overflowingTiming.getBytes(StandardCharsets.UTF_8)),
        400,
        0,
        "invalid");
    checkResponse(
        endpoint.post(
            "different",
            record(1_700_000_000_000L).toJson().getBytes(StandardCharsets.UTF_8)),
        400,
        0,
        "invalid");
    checkResponse(
        endpoint.post(
            SHARED_SESSION,
            new byte[CoordinationHttpEndpoint.MAXIMUM_REQUEST_BODY_BYTES + 1]),
        400,
        0,
        "invalid");
  }

  private static CoordinationHttpEndpoint endpoint() {
    return new CoordinationHttpEndpoint(new CoordinationRecordStore(new MemoryStore()));
  }

  private static PairedCoordinationRecord record(long recordedAtEpochMillis) {
    NodeEvidence down =
        new NodeEvidence(
            CaptureRole.DOWN_THE_LINE,
            "dtl-node",
            "dtl-local",
            10_000_000,
            200,
            9_000_000,
            500,
            1_000_000,
            300,
            400,
            800,
            3,
            "local_audio");
    NodeEvidence face =
        new NodeEvidence(
            CaptureRole.FACE_ON,
            "face-node",
            "face-local",
            20_001_000,
            250,
            9_001_000,
            500,
            11_000_000,
            250,
            500,
            900,
            4,
            "local_audio");
    return PairedCoordinationRecord.create(
        SHARED_SESSION, recordedAtEpochMillis, down, face);
  }

  private static void checkResponse(
      CoordinationHttpEndpoint.Response response,
      int status,
      long revision,
      String storeStatus) {
    check(response.statusCode() == status, "HTTP status " + status);
    check(response.revision() == revision, "revision " + revision);
    check(response.storeStatus().equals(storeStatus), "store status " + storeStatus);
  }

  private static void check(boolean condition, String label) {
    if (!condition) {
      throw new AssertionError(label);
    }
  }

  private static final class MemoryStore implements AtomicTextStore {
    private final Map<String, VersionedText> values = new HashMap<>();

    @Override
    public synchronized Optional<VersionedText> read(String key) {
      return Optional.ofNullable(values.get(key));
    }

    @Override
    public synchronized long compareAndSet(String key, long expectedRevision, String value) {
      long currentRevision = values.containsKey(key) ? values.get(key).revision() : 0;
      if (currentRevision != expectedRevision) {
        return CONFLICT;
      }
      long revision = currentRevision + 1;
      values.put(key, new VersionedText(revision, value));
      return revision;
    }
  }
}
