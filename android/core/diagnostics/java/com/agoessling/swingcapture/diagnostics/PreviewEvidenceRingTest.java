package com.agoessling.swingcapture.diagnostics;

import com.agoessling.swingcapture.diagnostics.PreviewEvidence.ControllerState;
import com.agoessling.swingcapture.diagnostics.PreviewEvidenceRing.AppendRejectedException;
import com.agoessling.swingcapture.diagnostics.PreviewEvidenceRing.RejectionKind;
import com.agoessling.swingcapture.diagnostics.PreviewEvidenceRing.Snapshot;

/** Deterministic duration, count, byte, ordering, selection, and immutability coverage. */
public final class PreviewEvidenceRingTest {
  private PreviewEvidenceRingTest() {}

  public static void main(String[] arguments) {
    exactFramesAreImmutableAndSnapshotIsDetached();
    oldestEntriesAreEvictedByEveryIndependentBound();
    rejectedAppendsAreTypedAndNonmutating();
    halfOpenSnapshotPreservesTimestampOrdering();
    lowerCadenceFramesAttachToFiveHzObservations();
    malformedEvidenceAndBoundsAreRejected();
  }

  private static void exactFramesAreImmutableAndSnapshotIsDetached() {
    PreviewEvidenceRing ring = new PreviewEvidenceRing(10_000, 10, 100);
    byte[] frame = {1, 2, 3};
    PreviewEvidence first = evidence(100, frame);
    frame[0] = 99;
    ring.append(first);
    Snapshot snapshot = ring.snapshot();
    check(snapshot.entryCount() == 1, "one entry snapshotted");
    check(snapshot.compressedBytes() == 3, "snapshot bytes");
    byte[] exported = snapshot.entryAt(0).copyCompressedFrame();
    check(exported[0] == 1, "constructor detached input");
    exported[0] = 88;
    check(snapshot.entryAt(0).copyCompressedFrame()[0] == 1, "accessor returns copy");

    ring.append(evidence(200, new byte[] {4}));
    check(snapshot.entryCount() == 1, "snapshot ordering detached from ring");
    expectUnsupported(() -> snapshot.entries().clear(), "snapshot list mutation");
  }

  private static void oldestEntriesAreEvictedByEveryIndependentBound() {
    PreviewEvidenceRing byCount = new PreviewEvidenceRing(100, 2, 100);
    byCount.append(evidence(1, 2));
    byCount.append(evidence(2, 2));
    byCount.append(evidence(3, 2));
    checkTimestamps(byCount.snapshot(), 2, 3);

    PreviewEvidenceRing byBytes = new PreviewEvidenceRing(100, 10, 5);
    byBytes.append(evidence(1, 3));
    byBytes.append(evidence(2, 3));
    checkTimestamps(byBytes.snapshot(), 2);
    check(byBytes.retainedCompressedBytes() == 3, "byte retention accounting");

    PreviewEvidenceRing byDuration = new PreviewEvidenceRing(10, 10, 100);
    byDuration.append(evidence(5, 1));
    byDuration.append(evidence(15, 1));
    checkTimestamps(byDuration.snapshot(), 5, 15);
    byDuration.append(evidence(16, 1));
    checkTimestamps(byDuration.snapshot(), 15, 16);
  }

  private static void rejectedAppendsAreTypedAndNonmutating() {
    PreviewEvidenceRing ring = new PreviewEvidenceRing(100, 10, 4);
    ring.append(evidence(10, 2));
    AppendRejectedException duplicate = expectRejected(() -> ring.append(evidence(10, 1)));
    check(duplicate.kind() == RejectionKind.NONMONOTONIC_TIMESTAMP, "duplicate kind");
    AppendRejectedException backwards = expectRejected(() -> ring.append(evidence(9, 1)));
    check(backwards.kind() == RejectionKind.NONMONOTONIC_TIMESTAMP, "backwards kind");
    AppendRejectedException oversize = expectRejected(() -> ring.append(evidence(11, 5)));
    check(oversize.kind() == RejectionKind.ENTRY_EXCEEDS_BYTE_CAPACITY, "oversize kind");
    checkTimestamps(ring.snapshot(), 10);
    check(ring.retainedCompressedBytes() == 2, "rejections do not mutate bytes");
  }

  private static void halfOpenSnapshotPreservesTimestampOrdering() {
    PreviewEvidenceRing ring = new PreviewEvidenceRing(100, 10, 100);
    ring.append(evidence(10, 1));
    ring.append(evidence(20, 2));
    ring.append(evidence(30, 3));
    ring.append(evidence(40, 4));
    Snapshot snapshot = ring.snapshot(20, 40);
    check(snapshot.firstTimestampInclusive() == 20, "snapshot first bound");
    check(snapshot.endTimestampExclusive() == 40, "snapshot end bound");
    check(snapshot.compressedBytes() == 5, "selected byte total");
    checkTimestamps(snapshot, 20, 30);
    check(ring.snapshot(21, 30).entryCount() == 0, "empty selection is valid");
  }

  private static void lowerCadenceFramesAttachToFiveHzObservations() {
    PreviewEvidenceRing ring = new PreviewEvidenceRing(10_000, 10, 100);
    for (long timestamp = 0; timestamp <= 800; timestamp += 200) {
      ring.append(evidence(timestamp, new byte[0]));
    }
    check(ring.snapshot().entryCount() == 5, "all five-Hz observations retained");
    check(ring.snapshot().compressedBytes() == 0, "observation rows need no JPEG");
    check(ring.attachCompressedFrame(0, new byte[] {1, 2, 3}), "first JPEG attached");
    check(ring.attachCompressedFrame(800, new byte[] {4, 5}), "arm JPEG attached");
    check(!ring.attachCompressedFrame(1_000, new byte[] {9}), "evicted/unknown row rejected");
    Snapshot snapshot = ring.snapshot();
    check(snapshot.entryCount() == 5, "JPEG attachment preserves observation count");
    check(snapshot.compressedBytes() == 5, "JPEG attachment byte accounting");
    check(snapshot.entryAt(0).hasCompressedFrame(), "first JPEG present");
    check(!snapshot.entryAt(1).hasCompressedFrame(), "intermediate observation is trace-only");
    check(snapshot.entryAt(4).hasCompressedFrame(), "arm JPEG present");
  }

  private static void malformedEvidenceAndBoundsAreRejected() {
    expectIllegalArgument(() -> new PreviewEvidenceRing(0, 1, 1), "zero duration");
    expectIllegalArgument(() -> new PreviewEvidenceRing(1, 0, 1), "zero count");
    expectIllegalArgument(() -> new PreviewEvidenceRing(1, 1, 0), "zero bytes");
    PreviewEvidenceRing ring = new PreviewEvidenceRing(1, 1, 1);
    expectIllegalArgument(() -> ring.snapshot(-1, 1), "negative snapshot");
    expectIllegalArgument(() -> ring.snapshot(1, 1), "empty snapshot");
    expectIllegalArgument(() -> evidence(-1, 1), "negative timestamp");
    expectIllegalArgument(() -> evidence(Long.MAX_VALUE, 1), "unrepresentable end");
    check(!evidence(1, 0).hasCompressedFrame(), "trace-only evidence accepted");
    expectIllegalArgument(
        () -> evidence(1, 0).withCompressedFrame(new byte[0]),
        "empty attached frame");
    expectIllegalArgument(
        () ->
            new PreviewEvidence(
                1,
                new byte[] {1},
                "model",
                1,
                Double.NaN,
                0.5,
                0.1,
                true,
                ControllerState.MONITORING,
                "reason"),
        "NaN confidence");
    expectIllegalArgument(
        () ->
            new PreviewEvidence(
                1,
                new byte[] {1},
                "model",
                1,
                0.5,
                1.1,
                0.1,
                true,
                ControllerState.MONITORING,
                "reason"),
        "confidence outside unit interval");
    expectIllegalArgument(
        () ->
            new PreviewEvidence(
                1,
                new byte[] {1},
                "model",
                1,
                0.5,
                0.5,
                Double.POSITIVE_INFINITY,
                true,
                ControllerState.MONITORING,
                "reason"),
        "infinite motion");
  }

  private static PreviewEvidence evidence(long timestamp, int frameBytes) {
    byte[] frame = new byte[frameBytes];
    for (int index = 0; index < frame.length; ++index) {
      frame[index] = (byte) (timestamp + index);
    }
    return evidence(timestamp, frame);
  }

  private static PreviewEvidence evidence(long timestamp, byte[] frame) {
    return new PreviewEvidence(
        timestamp,
        frame,
        "mediapipe-pose-v1",
        12_000_000,
        0.8,
        0.7,
        0.12,
        true,
        ControllerState.QUALIFYING,
        "address candidate");
  }

  private static void checkTimestamps(Snapshot snapshot, long... expected) {
    check(snapshot.entryCount() == expected.length, "timestamp count");
    for (int index = 0; index < expected.length; ++index) {
      check(snapshot.entryAt(index).timestampBoottimeNanos() == expected[index],
          "timestamp " + index);
    }
  }

  private static AppendRejectedException expectRejected(Action action) {
    try {
      action.run();
    } catch (AppendRejectedException expected) {
      return expected;
    }
    throw new AssertionError("expected typed append rejection");
  }

  private static void expectIllegalArgument(Action action, String message) {
    try {
      action.run();
    } catch (IllegalArgumentException expected) {
      return;
    }
    throw new AssertionError(message + " was accepted");
  }

  private static void expectUnsupported(Action action, String message) {
    try {
      action.run();
    } catch (UnsupportedOperationException expected) {
      return;
    }
    throw new AssertionError(message + " was accepted");
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
}
