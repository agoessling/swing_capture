package com.agoessling.swingcapture.retention;

import com.agoessling.swingcapture.retention.EncodedAccessUnitRetention.AccessUnitMetadata;
import com.agoessling.swingcapture.retention.EncodedAccessUnitRetention.CaptureState;
import com.agoessling.swingcapture.retention.EncodedAccessUnitRetention.ContinuityDiagnostic;
import com.agoessling.swingcapture.retention.EncodedAccessUnitRetention.Failure;
import com.agoessling.swingcapture.retention.EncodedAccessUnitRetention.Limits;
import com.agoessling.swingcapture.retention.EncodedAccessUnitRetention.RetentionException;
import com.agoessling.swingcapture.retention.EncodedAccessUnitRetention.Snapshot;
import com.agoessling.swingcapture.retention.EncodedAccessUnitRetention.SnapshotMetadata;
import com.agoessling.swingcapture.retention.EncodedAccessUnitRetention.TriggerResult;
import com.agoessling.swingcapture.retention.EncodedAccessUnitRetention.TriggerStatus;
import java.nio.ByteBuffer;

/** Deterministic host tests for fixed-memory encoded access-unit retention. */
public final class EncodedAccessUnitRetentionTest {
  private static final long MS = 1_000_000L;

  private EncodedAccessUnitRetentionTest() {}

  public static void main(String[] arguments) throws Exception {
    liveRingWrapsWithinFixedBounds();
    payloadCopySpansFixedBlocks();
    snapshotIncludesPrecedingIdrAndPostRoll();
    snapshotCrossesOffFramePostRollBoundary();
    delayedTriggerIncludesExistingBoundaryFrame();
    snapshotOwnershipSurvivesRingOverwrite();
    triggerAdmissionReportsMissingHistoryAndIdr();
    startupTruncationRejectsMissingIdrAndTooLittleHistory();
    startupTruncationRequiresUnbrokenContinuityEpoch();
    startupTruncationDoesNotUseRolledWindow();
    startupTruncationAcceptsExactThresholdAndReportsActualHistory();
    startupOptInMatchesNormalPolicyOnceFullHistoryExists();
    startupTruncationPreservesPostRollAndCooldown();
    continuityFailuresAreExplicitAndAbortCapture();
    sensorContinuityGateUsesSuppliedSensorTimestamps();
    heldSnapshotCanExhaustTheBytePool();
    snapshotCapacityFailureIsExplicit();
    cooldownAndSnapshotPoolBoundBackToBackTriggers();
  }

  private static void liveRingWrapsWithinFixedBounds() throws Exception {
    EncodedAccessUnitRetention retention =
        new EncodedAccessUnitRetention(
            new Limits(64, 8, 100 * MS, 5, 1, 60 * MS, 20 * MS, 10 * MS, 20 * MS, 20 * MS));
    for (int ordinal = 0; ordinal < 10; ++ordinal) {
      ByteBuffer source = payload(ordinal, 8);
      int originalPosition = source.position();
      append(retention, ordinal, ordinal * 10 * MS, ordinal == 0, source);
      check(source.position() == originalPosition, "append must not mutate source position");
    }
    check(retention.retainedAccessUnitCount() == 5, "access-unit bound");
    check(retention.oldestRetainedOrdinal() == 5, "ring wrap ordinal");
    check(retention.oldestRetainedSensorTimestampNs() == 5 * 10 * MS,
        "ring wrap oldest timestamp");
    check(retention.newestRetainedSensorTimestampNs() == 9 * 10 * MS,
        "ring wrap newest timestamp");
    check(retention.retainedBytes() == 40, "retained byte accounting");
    check(retention.freeBytes() == 24, "fixed byte pool accounting");
  }

  private static void payloadCopySpansFixedBlocks() throws Exception {
    EncodedAccessUnitRetention retention =
        new EncodedAccessUnitRetention(
            new Limits(64, 8, 10 * MS, 4, 1, 0, 0, 0, 10 * MS, 10 * MS));
    ByteBuffer source = payload(23, 13);
    int originalPosition = source.position();
    append(retention, 0, 0, true, source);
    check(source.position() == originalPosition, "multi-block append preserves source position");
    check(retention.trigger(0).accepted(), "zero-window trigger accepted");
    Snapshot snapshot = requireSnapshot(retention);
    ByteBuffer destination = ByteBuffer.allocate(13);
    check(snapshot.copyPayload(0, destination) == 13, "multi-block payload size");
    destination.flip();
    while (destination.hasRemaining()) {
      check((destination.get() & 0xff) == 23, "multi-block payload contents");
    }
    snapshot.close();
  }

  private static void snapshotIncludesPrecedingIdrAndPostRoll() throws Exception {
    EncodedAccessUnitRetention retention = productionLikeRetention(16_384, 64, 2);
    appendRange(retention, 0, 20, 100 * MS, 5);

    TriggerResult accepted = retention.trigger(2_000 * MS);
    check(accepted.accepted(), "nominal trigger accepted");
    check(retention.captureState() == CaptureState.CAPTURING, "post-roll capture active");
    check(
        retention.trigger(2_010 * MS).status() == TriggerStatus.CAPTURE_ACTIVE,
        "trigger rejected during post-roll");

    appendRange(retention, 21, 25, 100 * MS, 5);
    check(retention.captureState() == CaptureState.COOLDOWN, "capture enters cooldown");
    Snapshot snapshot = requireSnapshot(retention);
    check(snapshot.captureId() == accepted.captureId(), "capture identifier preserved");
    check(snapshot.triggerSensorTimestampNs() == 2_000 * MS, "trigger timestamp preserved");
    check(snapshot.accessUnitCount() == 21, "IDR-backed 1.4s pre plus 0.5s post count");
    AccessUnitMetadata first = snapshot.metadata(0);
    AccessUnitMetadata last = snapshot.metadata(snapshot.accessUnitCount() - 1);
    check(first.ordinal() == 5 && first.idr(), "snapshot starts at preceding IDR");
    check(first.sensorTimestampNs() == 500 * MS, "IDR backs up before pre-roll boundary");
    check(last.ordinal() == 25 && last.sensorTimestampNs() == 2_500 * MS, "post-roll end");
    snapshot.close();
    expectIllegalState(snapshot::accessUnitCount, "closed lease rejected");
  }

  private static void snapshotCrossesOffFramePostRollBoundary() throws Exception {
    EncodedAccessUnitRetention retention = productionLikeRetention(16_384, 64, 2);
    appendRange(retention, 0, 20, 100 * MS, 5);

    TriggerResult accepted = retention.trigger(2_050 * MS);
    check(accepted.accepted(), "off-frame trigger accepted");
    appendRange(retention, 21, 26, 100 * MS, 5);

    Snapshot snapshot = requireSnapshot(retention);
    AccessUnitMetadata last = snapshot.metadata(snapshot.accessUnitCount() - 1);
    check(last.ordinal() == 26, "off-frame post-roll retains crossing frame");
    check(last.sensorTimestampNs() == 2_600 * MS, "off-frame post-roll reaches endpoint");
    check(
        last.sensorTimestampNs() >= snapshot.triggerSensorTimestampNs() + 500 * MS,
        "off-frame post-roll is never shortened");
    snapshot.close();
  }

  private static void delayedTriggerIncludesExistingBoundaryFrame() throws Exception {
    EncodedAccessUnitRetention retention = productionLikeRetention(16_384, 64, 2);
    appendRange(retention, 0, 26, 100 * MS, 5);

    TriggerResult accepted = retention.trigger(2_050 * MS);
    check(accepted.accepted(), "delayed off-frame trigger accepted");

    Snapshot snapshot = requireSnapshot(retention);
    AccessUnitMetadata last = snapshot.metadata(snapshot.accessUnitCount() - 1);
    check(last.ordinal() == 26, "delayed snapshot includes existing crossing frame");
    check(last.sensorTimestampNs() == 2_600 * MS, "delayed snapshot reaches endpoint");
    snapshot.close();
  }

  private static void snapshotOwnershipSurvivesRingOverwrite() throws Exception {
    EncodedAccessUnitRetention retention = productionLikeRetention(32_768, 64, 2);
    appendRange(retention, 0, 25, 100 * MS, 5);
    TriggerResult trigger = retention.trigger(2_000 * MS);
    check(trigger.accepted(), "delayed trigger accepted with existing post-roll");
    Snapshot snapshot = requireSnapshot(retention);

    appendRange(retention, 26, 70, 100 * MS, 5);
    check(retention.oldestRetainedOrdinal() > snapshot.metadata(0).ordinal(), "live ring overwrote range");
    ByteBuffer destination = ByteBuffer.allocate(snapshot.metadata(0).payloadBytes());
    snapshot.copyPayload(0, destination);
    destination.flip();
    check((destination.get() & 0xff) == 5, "snapshot payload survives ring eviction");

    snapshot.close();
    expectIllegalState(() -> snapshot.metadata(0), "released generation rejected");
  }

  private static void triggerAdmissionReportsMissingHistoryAndIdr() throws Exception {
    EncodedAccessUnitRetention shortHistory = productionLikeRetention(8_192, 64, 1);
    appendRange(shortHistory, 10, 20, 100 * MS, 5);
    check(
        shortHistory.trigger(2_000 * MS).status() == TriggerStatus.INSUFFICIENT_PRE_ROLL,
        "short pre-roll rejected");

    EncodedAccessUnitRetention noIdr = productionLikeRetention(8_192, 64, 1);
    for (int ordinal = 0; ordinal <= 20; ++ordinal) {
      append(noIdr, ordinal, ordinal * 100 * MS, false, payload(ordinal, 8));
    }
    check(
        noIdr.trigger(2_000 * MS).status() == TriggerStatus.NO_PRECEDING_IDR,
        "missing IDR rejected explicitly");

    EncodedAccessUnitRetention staleFrame = productionLikeRetention(8_192, 64, 1);
    appendRange(staleFrame, 0, 20, 100 * MS, 5);
    check(
        staleFrame.trigger(2_200 * MS).status() == TriggerStatus.NO_FRAME_AT_TRIGGER,
        "stale latest frame rejected explicitly");
  }

  private static void startupTruncationRejectsMissingIdrAndTooLittleHistory()
      throws Exception {
    EncodedAccessUnitRetention noIdr = productionLikeRetention(8_192, 64, 1);
    for (int ordinal = 0; ordinal <= 6; ++ordinal) {
      append(noIdr, ordinal, ordinal * 100 * MS, false, payload(ordinal, 8));
    }
    check(
        noIdr.triggerAllowingStartupTruncatedPreRoll(600 * MS, 500 * MS).status()
            == TriggerStatus.NO_PRECEDING_IDR,
        "startup truncation remains IDR-backed");

    EncodedAccessUnitRetention tooShort = productionLikeRetention(8_192, 64, 1);
    appendRange(tooShort, 0, 4, 100 * MS, 5);
    check(
        tooShort.triggerAllowingStartupTruncatedPreRoll(400 * MS, 500 * MS).status()
            == TriggerStatus.INSUFFICIENT_PRE_ROLL,
        "startup truncation enforces caller minimum");
    check(
        tooShort.trigger(400 * MS).status() == TriggerStatus.INSUFFICIENT_PRE_ROLL,
        "default trigger still requires configured pre-roll");
  }

  private static void startupTruncationRequiresUnbrokenContinuityEpoch() throws Exception {
    EncodedAccessUnitRetention retention = productionLikeRetention(8_192, 64, 1);
    appendRange(retention, 0, 6, 100 * MS, 5);
    expectRetentionFailure(
        () -> append(retention, 8, 700 * MS, false, payload(8, 8)), Failure.ORDINAL_GAP);
    check(
        retention.triggerAllowingStartupTruncatedPreRoll(600 * MS, 500 * MS).status()
            == TriggerStatus.GAP_IN_RETAINED_WINDOW,
        "unreset continuity failure disables startup admission");

    retention.resetContinuity();
    appendRange(retention, 8, 14, 100 * MS, 1);
    check(
        retention.triggerAllowingStartupTruncatedPreRoll(1_400 * MS, 500 * MS).accepted(),
        "new continuity epoch can use startup admission");
  }

  private static void startupTruncationDoesNotUseRolledWindow() throws Exception {
    EncodedAccessUnitRetention retention = productionLikeRetention(8_192, 8, 1);
    appendRange(retention, 0, 8, 100 * MS, 5);
    check(retention.oldestRetainedOrdinal() == 1, "test rolled past encoder startup");
    check(
        retention.triggerAllowingStartupTruncatedPreRoll(800 * MS, 200 * MS).status()
            == TriggerStatus.INSUFFICIENT_PRE_ROLL,
        "startup opt-in never becomes a shorter rolling-window policy");
  }

  private static void startupTruncationAcceptsExactThresholdAndReportsActualHistory()
      throws Exception {
    EncodedAccessUnitRetention retention = productionLikeRetention(8_192, 64, 1);
    appendRange(retention, 0, 6, 100 * MS, 5);

    TriggerResult accepted =
        retention.triggerAllowingStartupTruncatedPreRoll(600 * MS, 600 * MS);
    check(accepted.accepted(), "exact startup-history threshold accepted");
    appendRange(retention, 7, 11, 100 * MS, 5);

    Snapshot snapshot = requireSnapshot(retention);
    SnapshotMetadata metadata = snapshot.snapshotMetadata();
    check(metadata.triggerSensorTimestampNs() == 600 * MS, "metadata trigger timestamp");
    check(
        metadata.firstAccessUnitSensorTimestampNs() == 0,
        "metadata identifies IDR-backed snapshot start");
    check(
        metadata.configuredPreRollNs() == EncodedAccessUnitRetention.SWING_PRE_ROLL_NS,
        "metadata preserves configured normal pre-roll");
    check(
        metadata.minimumRequiredPreRollNs() == 600 * MS,
        "metadata preserves startup admission threshold");
    check(metadata.actualPreRollNs() == 600 * MS, "metadata exposes actual truncated pre-roll");
    check(metadata.startupPreRollTruncated(), "metadata marks startup truncation");
    check(snapshot.metadata(0).idr(), "truncated snapshot begins with IDR");
    snapshot.close();
  }

  private static void startupOptInMatchesNormalPolicyOnceFullHistoryExists() throws Exception {
    EncodedAccessUnitRetention normal = productionLikeRetention(16_384, 64, 1);
    EncodedAccessUnitRetention optedIn = productionLikeRetention(16_384, 64, 1);
    appendRange(normal, 0, 25, 100 * MS, 5);
    appendRange(optedIn, 0, 25, 100 * MS, 5);

    check(normal.trigger(2_000 * MS).accepted(), "normal full-history trigger accepted");
    check(
        optedIn.triggerAllowingStartupTruncatedPreRoll(2_000 * MS, 600 * MS).accepted(),
        "opt-in full-history trigger accepted");

    Snapshot normalSnapshot = requireSnapshot(normal);
    Snapshot optedInSnapshot = requireSnapshot(optedIn);
    check(
        normalSnapshot.accessUnitCount() == optedInSnapshot.accessUnitCount(),
        "full-history access-unit count equivalent");
    for (int index = 0; index < normalSnapshot.accessUnitCount(); ++index) {
      check(
          normalSnapshot.metadata(index).equals(optedInSnapshot.metadata(index)),
          "full-history access-unit selection equivalent");
    }
    SnapshotMetadata normalMetadata = normalSnapshot.snapshotMetadata();
    SnapshotMetadata optedInMetadata = optedInSnapshot.snapshotMetadata();
    check(normalMetadata.equals(optedInMetadata), "full-history snapshot metadata equivalent");
    check(!optedInMetadata.startupPreRollTruncated(), "full history is never marked truncated");
    check(
        optedInMetadata.minimumRequiredPreRollNs()
            == EncodedAccessUnitRetention.SWING_PRE_ROLL_NS,
        "full-history admission preserves normal minimum");
    normalSnapshot.close();
    optedInSnapshot.close();
  }

  private static void startupTruncationPreservesPostRollAndCooldown() throws Exception {
    EncodedAccessUnitRetention retention = productionLikeRetention(8_192, 64, 2);
    appendRange(retention, 0, 6, 100 * MS, 5);
    check(
        retention.triggerAllowingStartupTruncatedPreRoll(600 * MS, 500 * MS).accepted(),
        "startup post-roll trigger accepted");
    check(retention.captureState() == CaptureState.CAPTURING, "startup capture enters post-roll");
    check(
        retention.trigger(700 * MS).status() == TriggerStatus.CAPTURE_ACTIVE,
        "normal trigger cannot bypass startup capture");

    appendRange(retention, 7, 11, 100 * MS, 5);
    check(retention.captureState() == CaptureState.COOLDOWN, "startup capture enters cooldown");
    Snapshot snapshot = requireSnapshot(retention);
    AccessUnitMetadata last = snapshot.metadata(snapshot.accessUnitCount() - 1);
    check(last.sensorTimestampNs() == 1_100 * MS, "startup capture retains full post-roll");
    check(
        retention.triggerAllowingStartupTruncatedPreRoll(1_200 * MS, 500 * MS).status()
            == TriggerStatus.COOLDOWN,
        "startup opt-in cannot bypass cooldown");
    snapshot.close();
  }

  private static void continuityFailuresAreExplicitAndAbortCapture() throws Exception {
    EncodedAccessUnitRetention retention = productionLikeRetention(8_192, 64, 1);
    appendRange(retention, 0, 14, 100 * MS, 5);
    TriggerResult trigger = retention.trigger(1_400 * MS);
    check(trigger.accepted(), "capture active before ordinal gap");
    RetentionException ordinalGap =
        expectRetentionFailure(
            () -> append(retention, 16, 1_600 * MS, false, payload(16, 8)),
            Failure.ORDINAL_GAP);
    check(ordinalGap.captureId() == trigger.captureId(), "gap identifies aborted capture");
    ContinuityDiagnostic ordinalDiagnostic = ordinalGap.continuityDiagnostic().orElseThrow();
    check(ordinalDiagnostic.previous().ordinal() == 14, "ordinal gap previous ordinal");
    check(ordinalDiagnostic.current().ordinal() == 16, "ordinal gap current ordinal");
    check(ordinalDiagnostic.ordinalDelta() == 2, "ordinal gap delta");
    check(
        ordinalDiagnostic.presentationTimestampGapUs() == 200 * MS / 1_000,
        "ordinal gap presentation-time delta");
    check(
        ordinalDiagnostic.sensorTimestampGapNs() == 200 * MS,
        "ordinal gap sensor-time delta");
    check(retention.captureState() == CaptureState.IDLE, "gap aborts capture");

    retention.resetContinuity();
    append(retention, 16, 1_600 * MS, true, payload(16, 8));
    RetentionException sensorGap =
        expectRetentionFailure(
            () -> append(retention, 17, 1_800 * MS, false, payload(17, 8)),
            Failure.SENSOR_TIMESTAMP_GAP);
    ContinuityDiagnostic sensorDiagnostic = sensorGap.continuityDiagnostic().orElseThrow();
    check(
        sensorDiagnostic.previous().sensorTimestampNs() == 1_600 * MS,
        "sensor gap previous timestamp");
    check(
        sensorDiagnostic.current().sensorTimestampNs() == 1_800 * MS,
        "sensor gap current timestamp");
    check(sensorDiagnostic.sensorTimestampGapNs() == 200 * MS, "sensor gap delta");
    check(
        sensorDiagnostic.maximumSensorTimestampGapNs() == 150 * MS,
        "sensor gap configured maximum");
    check(
        sensorGap.getMessage().contains("sensor_timestamp_gap_ns=200000000"),
        "sensor gap message remains actionable without typed logging");
  }

  private static void sensorContinuityGateUsesSuppliedSensorTimestamps() throws Exception {
    EncodedAccessUnitRetention retention =
        new EncodedAccessUnitRetention(
            new Limits(256, 8, 100 * MS, 8, 1, 0, 0, 0, 20 * MS, 20 * MS));

    retention.append(0, 1_000, 1_000_000, 7, true, payload(0, 8));
    retention.append(1, 26_000, 5_167_000, 7, false, payload(1, 8));
    retention.append(2, 30_167, 25_167_000, 7, false, payload(2, 8));

    check(
        retention.retainedAccessUnitCount() == 3,
        "large presentation-time gap alone does not trip the sensor gate");
    RetentionException gap =
        expectRetentionFailure(
            () -> retention.append(3, 34_334, 45_168_000, 7, false, payload(3, 8)),
            Failure.SENSOR_TIMESTAMP_GAP);
    ContinuityDiagnostic diagnostic = gap.continuityDiagnostic().orElseThrow();
    check(
        diagnostic.presentationTimestampGapUs() == 4_167,
        "diagnostic preserves nominal encoded PTS gap");
    check(
        diagnostic.sensorTimestampGapNs() == 20_001_000,
        "diagnostic identifies supplied sensor-time gap");
    check(
        diagnostic.maximumSensorTimestampGapNs() == 20 * MS,
        "exact sensor continuity policy is reported");
  }

  private static void heldSnapshotCanExhaustTheBytePool() throws Exception {
    EncodedAccessUnitRetention retention =
        new EncodedAccessUnitRetention(
            new Limits(32, 8, 60 * MS, 8, 1, 20 * MS, 10 * MS, 0, 10 * MS, 10 * MS));
    appendRange(retention, 0, 2, 10 * MS, 10);
    check(retention.trigger(20 * MS).accepted(), "small-pool trigger accepted");
    append(retention, 3, 30 * MS, false, payload(3, 8));
    Snapshot snapshot = requireSnapshot(retention);
    check(retention.freeBytes() == 0, "snapshot fills byte pool");

    RetentionException exhausted =
        expectRetentionFailure(
            () -> append(retention, 4, 40 * MS, false, payload(4, 8)),
            Failure.BYTE_POOL_EXHAUSTED);
    check(exhausted.captureId() == 0, "completed snapshot owns exhaustion");
    check(
        exhausted.continuityDiagnostic().isEmpty(),
        "resource failure has no synthetic continuity evidence");
    check(snapshot.metadata(0).ordinal() == 0, "exhaustion cannot overwrite snapshot metadata");

    snapshot.close();
    append(retention, 4, 40 * MS, false, payload(4, 8));
    check(retention.retainedAccessUnitCount() == 1, "append recovers after lease release");
  }

  private static void snapshotCapacityFailureIsExplicit() throws Exception {
    EncodedAccessUnitRetention retention =
        new EncodedAccessUnitRetention(
            new Limits(256, 8, 100 * MS, 5, 1, 20 * MS, 40 * MS, 0, 10 * MS, 10 * MS));
    appendRange(retention, 0, 2, 10 * MS, 10);
    TriggerResult trigger = retention.trigger(20 * MS);
    check(trigger.accepted(), "capacity test trigger accepted");
    append(retention, 3, 30 * MS, false, payload(3, 8));
    append(retention, 4, 40 * MS, false, payload(4, 8));
    RetentionException exhausted =
        expectRetentionFailure(
            () -> append(retention, 5, 50 * MS, false, payload(5, 8)),
            Failure.SNAPSHOT_CAPACITY_EXHAUSTED);
    check(exhausted.captureId() == trigger.captureId(), "capacity failure identifies capture");
    append(retention, 5, 50 * MS, false, payload(5, 8));
    check(retention.oldestRetainedOrdinal() == 1, "failed capture did not partially append payload");
  }

  private static void cooldownAndSnapshotPoolBoundBackToBackTriggers() throws Exception {
    EncodedAccessUnitRetention retention = productionLikeRetention(16_384, 64, 1);
    appendRange(retention, 0, 25, 100 * MS, 5);
    TriggerResult first = retention.trigger(2_000 * MS);
    check(first.accepted(), "first delayed trigger accepted");
    check(
        retention.trigger(2_600 * MS).status() == TriggerStatus.COOLDOWN,
        "back-to-back trigger rejected during cooldown");
    appendRange(retention, 26, 30, 100 * MS, 5);
    check(retention.captureState() == CaptureState.IDLE, "cooldown advanced by stream time");
    check(
        retention.trigger(3_000 * MS).status() == TriggerStatus.SNAPSHOT_POOL_EXHAUSTED,
        "unreleased completed snapshot bounds back-to-back ownership");

    Snapshot completed = requireSnapshot(retention);
    completed.close();
    check(retention.trigger(3_000 * MS).accepted(), "trigger admitted after snapshot release");
  }

  private static EncodedAccessUnitRetention productionLikeRetention(
      int maxBytes, int maxAccessUnits, int maxSnapshots) {
    return new EncodedAccessUnitRetention(
        new Limits(
            maxBytes,
            8,
            3_000 * MS,
            maxAccessUnits,
            maxSnapshots,
            EncodedAccessUnitRetention.SWING_PRE_ROLL_NS,
            EncodedAccessUnitRetention.SWING_POST_ROLL_NS,
            200 * MS,
            150 * MS,
            150 * MS));
  }

  private static void appendRange(
      EncodedAccessUnitRetention retention,
      int firstOrdinal,
      int lastOrdinal,
      long intervalNs,
      int idrInterval)
      throws RetentionException {
    for (int ordinal = firstOrdinal; ordinal <= lastOrdinal; ++ordinal) {
      append(
          retention,
          ordinal,
          ordinal * intervalNs,
          ordinal % idrInterval == 0,
          payload(ordinal, 8));
    }
  }

  private static void append(
      EncodedAccessUnitRetention retention,
      long ordinal,
      long sensorTimestampNs,
      boolean idr,
      ByteBuffer payload)
      throws RetentionException {
    retention.append(ordinal, sensorTimestampNs / 1_000, sensorTimestampNs, 7, idr, payload);
  }

  private static ByteBuffer payload(long ordinal, int bytes) {
    byte[] payload = new byte[bytes + 2];
    for (int index = 1; index <= bytes; ++index) {
      payload[index] = (byte) ordinal;
    }
    return ByteBuffer.wrap(payload, 1, bytes);
  }

  private static Snapshot requireSnapshot(EncodedAccessUnitRetention retention) {
    Snapshot snapshot = retention.pollCompletedSnapshot();
    check(snapshot != null, "completed snapshot expected");
    return snapshot;
  }

  private static RetentionException expectRetentionFailure(
      ThrowingAction action, Failure expectedFailure) throws Exception {
    try {
      action.run();
    } catch (RetentionException failure) {
      check(failure.failure() == expectedFailure, "expected " + expectedFailure + " failure");
      return failure;
    }
    throw new AssertionError("expected retention failure " + expectedFailure);
  }

  private static void expectIllegalState(Action action, String message) {
    try {
      action.run();
    } catch (IllegalStateException expected) {
      return;
    }
    throw new AssertionError(message);
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }

  @FunctionalInterface
  private interface Action {
    void run();
  }

  @FunctionalInterface
  private interface ThrowingAction {
    void run() throws Exception;
  }
}
