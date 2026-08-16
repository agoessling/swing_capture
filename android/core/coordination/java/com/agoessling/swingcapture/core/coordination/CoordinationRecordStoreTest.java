package com.agoessling.swingcapture.core.coordination;

import com.agoessling.swingcapture.core.coordination.CoordinationRecordStore.StoreStatus;
import com.agoessling.swingcapture.core.coordination.PairedCoordinationRecord.NodeEvidence;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;
import java.util.Optional;

/** Deterministic contract, serialization, corruption, and atomic-publication tests. */
public final class CoordinationRecordStoreTest {
  private static final String SHARED_SESSION = "shared-42";
  private static final long RECORDED_AT_EPOCH_MILLIS = 1_700_000_000_000L;

  private CoordinationRecordStoreTest() {}

  public static void main(String[] arguments) throws Exception {
    canonicalJsonRoundTripsAllEvidence();
    valueObjectRejectsInconsistentEvidence();
    jsonParserRejectsMalformedOrUnboundedInput();
    storeIsImmutableAndIdempotent();
    competingAtomicWriterIsReportedDeterministically();
    corruptOrMiskeyedPersistenceIsRejected();
    invalidBackingStoreRevisionIsRejected();
  }

  private static void canonicalJsonRoundTripsAllEvidence() {
    PairedCoordinationRecord record = record(RECORDED_AT_EPOCH_MILLIS);
    String json = record.toJson();

    check(json.startsWith("{\"schema_version\":1,"), "canonical schema prefix");
    check(json.contains("\"shared_session_id\":\"shared-42\""), "shared session field");
    check(json.contains("\"status\":\"paired\""), "paired status field");
    check(json.contains("\"clock_offset_ns\":\"1000000\""), "64-bit values are strings");
    check(json.contains("\"clock_sample_count\":3"), "bounded count remains an integer");
    check(
        json.getBytes(StandardCharsets.UTF_8).length
            < PairedCoordinationRecord.MAXIMUM_SERIALIZED_BYTES,
        "nominal record remains bounded");
    check(PairedCoordinationRecord.fromJson(json).equals(record), "canonical round trip");
    check(
        PairedCoordinationRecord.fromJson(" \n" + json + "\t").equals(record),
        "insignificant JSON whitespace");

    String escapedIdentifier = json.replace("shared-42", "shared\\u002d42");
    check(
        PairedCoordinationRecord.fromJson(escapedIdentifier).equals(record),
        "valid JSON escapes decode before identifier validation");
    check(record.minimumTriggerSeparationNs() == 0, "overlapping trigger interval minimum");
    check(record.maximumTriggerSeparationNs() == 2_000, "trigger interval maximum");
  }

  private static void valueObjectRejectsInconsistentEvidence() {
    PairedCoordinationRecord record = record(RECORDED_AT_EPOCH_MILLIS);
    expectIllegalArgument(
        () ->
            new PairedCoordinationRecord(
                record.sharedSessionId(),
                record.recordedAtEpochMillis(),
                record.downTheLine(),
                record.faceOn(),
                1,
                record.maximumTriggerSeparationNs()),
        "derived separation mismatch");
    expectIllegalArgument(
        () ->
            PairedCoordinationRecord.create(
                record.sharedSessionId(),
                record.recordedAtEpochMillis(),
                record.downTheLine(),
                evidence(CaptureRole.FACE_ON, "dtl-node", "face-local", 20_001_000, 11_000_000)),
        "one node for both roles");
    expectIllegalArgument(
        () ->
            PairedCoordinationRecord.create(
                record.sharedSessionId(),
                record.recordedAtEpochMillis(),
                record.faceOn(),
                record.downTheLine()),
        "role reversal");
    expectIllegalArgument(
        () ->
            new NodeEvidence(
                CaptureRole.DOWN_THE_LINE,
                "node",
                "local",
                10_000,
                10,
                9_001,
                20,
                1_000,
                10,
                20,
                40,
                3,
                "local_audio"),
        "mapped timestamp mismatch");
    expectIllegalArgument(
        () ->
            new NodeEvidence(
                CaptureRole.DOWN_THE_LINE,
                "node",
                "local",
                10_000,
                10,
                9_000,
                20,
                1_000,
                10,
                20,
                40,
                65,
                "local_audio"),
        "clock sample bound");

    NodeTriggerReport unrelated =
        new NodeTriggerReport(CaptureRole.DOWN_THE_LINE, "node", "another-session", 10_000, 10);
    expectIllegalArgument(
        () ->
            NodeEvidence.map(
                SHARED_SESSION,
                unrelated,
                "local",
                "local_audio",
                new ClockOffsetEstimate("node", 1_000, 10, 20, 40, 3)),
        "shared session mismatch");
    expectIllegalArgument(
        () -> PairedCoordinationRecord.validateSharedSessionId("bad/session"),
        "unsafe storage key");
    expectIllegalArgument(
        () ->
            PairedCoordinationRecord.validateSharedSessionId(
                "x".repeat(PairedCoordinationRecord.MAXIMUM_IDENTIFIER_LENGTH + 1)),
        "storage key length");
  }

  private static void jsonParserRejectsMalformedOrUnboundedInput() {
    String json = record(RECORDED_AT_EPOCH_MILLIS).toJson();
    expectIllegalArgument(
        () ->
            PairedCoordinationRecord.fromJson(
                json.replace(
                    "\"status\":\"paired\"", "\"unexpected\":1,\"status\":\"paired\"")),
        "unknown schema field");
    expectIllegalArgument(
        () ->
            PairedCoordinationRecord.fromJson(
                json.replace(
                    "\"schema_version\":1",
                    "\"schema_version\":1,\"schema_version\":1")),
        "duplicate schema field");
    expectIllegalArgument(
        () ->
            PairedCoordinationRecord.fromJson(
                json.replace(
                    "\"mapped_coordinator_timestamp_ns\":\"9000000\"",
                    "\"mapped_coordinator_timestamp_ns\":\"9000001\"")),
        "serialized mapping mismatch");
    expectIllegalArgument(
        () ->
            PairedCoordinationRecord.fromJson(
                json.replace("\"clock_sample_count\":3", "\"clock_sample_count\":3.0")),
        "noninteger sample count");
    expectIllegalArgument(
        () -> PairedCoordinationRecord.fromJson("{" + " ".repeat(32 * 1024) + "}"),
        "serialized size bound");
    expectIllegalArgument(() -> PairedCoordinationRecord.fromJson(json + "{}"), "trailing JSON");
  }

  private static void storeIsImmutableAndIdempotent() throws Exception {
    InMemoryAtomicTextStore persistence = new InMemoryAtomicTextStore();
    CoordinationRecordStore store = new CoordinationRecordStore(persistence);
    PairedCoordinationRecord first = record(RECORDED_AT_EPOCH_MILLIS);

    CoordinationRecordStore.StoreResult stored = store.storeIfAbsent(first);
    check(stored.status() == StoreStatus.STORED, "first writer stores evidence");
    check(stored.stored().revision() == 1, "first revision");
    check(store.read(SHARED_SESSION).orElseThrow().record().equals(first), "stored read");

    CoordinationRecordStore.StoreResult replay = store.storeIfAbsent(first);
    check(replay.status() == StoreStatus.ALREADY_PRESENT, "exact replay is idempotent");
    check(persistence.compareAndSetCalls == 1, "idempotent replay performs no write");

    PairedCoordinationRecord conflicting = record(RECORDED_AT_EPOCH_MILLIS + 1);
    CoordinationRecordStore.StoreResult conflict = store.storeIfAbsent(conflicting);
    check(conflict.status() == StoreStatus.CONFLICT, "different immutable evidence conflicts");
    check(conflict.stored().record().equals(first), "first writer remains authoritative");
    check(persistence.compareAndSetCalls == 1, "known conflict performs no write");
    check(store.read("unused-session").isEmpty(), "missing session read");
  }

  private static void competingAtomicWriterIsReportedDeterministically() throws Exception {
    PairedCoordinationRecord requested = record(RECORDED_AT_EPOCH_MILLIS);
    PairedCoordinationRecord winner = record(RECORDED_AT_EPOCH_MILLIS + 1);
    RacingAtomicTextStore persistence = new RacingAtomicTextStore(winner.toJson());
    CoordinationRecordStore store = new CoordinationRecordStore(persistence);

    CoordinationRecordStore.StoreResult result = store.storeIfAbsent(requested);
    check(result.status() == StoreStatus.CONFLICT, "atomic race reports the winner");
    check(result.stored().record().equals(winner), "race winner is returned");
    check(result.stored().revision() == 7, "race winner revision preserved");
  }

  private static void corruptOrMiskeyedPersistenceIsRejected() {
    InMemoryAtomicTextStore corrupt = new InMemoryAtomicTextStore();
    corrupt.putRaw(SHARED_SESSION, 1, "not-json");
    expectIOException(
        () -> new CoordinationRecordStore(corrupt).read(SHARED_SESSION),
        "corrupt stored JSON");

    InMemoryAtomicTextStore miskeyed = new InMemoryAtomicTextStore();
    miskeyed.putRaw(SHARED_SESSION, 2, recordFor("another-session").toJson());
    expectIOException(
        () -> new CoordinationRecordStore(miskeyed).read(SHARED_SESSION),
        "miskeyed stored record");
  }

  private static void invalidBackingStoreRevisionIsRejected() {
    AtomicTextStore invalid =
        new AtomicTextStore() {
          @Override
          public Optional<VersionedText> read(String key) {
            return Optional.empty();
          }

          @Override
          public long compareAndSet(String key, long expectedRevision, String value) {
            return 0;
          }
        };
    expectIOException(
        () -> new CoordinationRecordStore(invalid).storeIfAbsent(record(RECORDED_AT_EPOCH_MILLIS)),
        "invalid successful revision");
  }

  private static PairedCoordinationRecord record(long recordedAtEpochMillis) {
    return recordFor(SHARED_SESSION, recordedAtEpochMillis);
  }

  private static PairedCoordinationRecord recordFor(String sharedSessionId) {
    return recordFor(sharedSessionId, RECORDED_AT_EPOCH_MILLIS);
  }

  private static PairedCoordinationRecord recordFor(
      String sharedSessionId, long recordedAtEpochMillis) {
    NodeTriggerReport downReport =
        new NodeTriggerReport(
            CaptureRole.DOWN_THE_LINE, "dtl-node", sharedSessionId, 10_000_000, 200);
    NodeTriggerReport faceReport =
        new NodeTriggerReport(
            CaptureRole.FACE_ON, "face-node", sharedSessionId, 20_001_000, 250);
    NodeEvidence down =
        NodeEvidence.map(
            sharedSessionId,
            downReport,
            "dtl-local",
            "local_audio",
            new ClockOffsetEstimate("dtl-node", 1_000_000, 300, 400, 800, 3));
    NodeEvidence face =
        NodeEvidence.map(
            sharedSessionId,
            faceReport,
            "face-local",
            "local_audio",
            new ClockOffsetEstimate("face-node", 11_000_000, 250, 500, 900, 3));
    return PairedCoordinationRecord.create(sharedSessionId, recordedAtEpochMillis, down, face);
  }

  private static NodeEvidence evidence(
      CaptureRole role,
      String nodeId,
      String localSessionId,
      long triggerTimestampNs,
      long clockOffsetNs) {
    return new NodeEvidence(
        role,
        nodeId,
        localSessionId,
        triggerTimestampNs,
        250,
        triggerTimestampNs - clockOffsetNs,
        500,
        clockOffsetNs,
        250,
        500,
        900,
        3,
        "local_audio");
  }

  private static void expectIllegalArgument(ThrowingAction action, String message) {
    try {
      action.run();
    } catch (IllegalArgumentException expected) {
      return;
    } catch (Exception unexpected) {
      throw new AssertionError(message + " threw an unexpected exception", unexpected);
    }
    throw new AssertionError(message + " was accepted");
  }

  private static void expectIOException(ThrowingAction action, String message) {
    try {
      action.run();
    } catch (IOException expected) {
      return;
    } catch (Exception unexpected) {
      throw new AssertionError(message + " threw an unexpected exception", unexpected);
    }
    throw new AssertionError(message + " was accepted");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }

  @FunctionalInterface
  private interface ThrowingAction {
    void run() throws Exception;
  }

  private static final class InMemoryAtomicTextStore implements AtomicTextStore {
    private final Map<String, VersionedText> values = new HashMap<>();
    private int compareAndSetCalls;

    @Override
    public synchronized Optional<VersionedText> read(String key) {
      return Optional.ofNullable(values.get(key));
    }

    @Override
    public synchronized long compareAndSet(
        String key, long expectedRevision, String value) {
      ++compareAndSetCalls;
      VersionedText current = values.get(key);
      long currentRevision = current == null ? MISSING_REVISION : current.revision();
      if (currentRevision != expectedRevision) {
        return CONFLICT;
      }
      long nextRevision = currentRevision + 1;
      values.put(key, new VersionedText(nextRevision, value));
      return nextRevision;
    }

    private synchronized void putRaw(String key, long revision, String value) {
      values.put(key, new VersionedText(revision, value));
    }
  }

  private static final class RacingAtomicTextStore implements AtomicTextStore {
    private final String winningValue;
    private VersionedText value;

    private RacingAtomicTextStore(String winningValue) {
      this.winningValue = winningValue;
    }

    @Override
    public Optional<VersionedText> read(String key) {
      return Optional.ofNullable(value);
    }

    @Override
    public long compareAndSet(String key, long expectedRevision, String requestedValue) {
      value = new VersionedText(7, winningValue);
      return CONFLICT;
    }
  }
}
