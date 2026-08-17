package com.agoessling.swingcapture.diagnostics;

import com.agoessling.swingcapture.diagnostics.DiagnosticAudioRing.AppendDiscontinuityException;
import com.agoessling.swingcapture.diagnostics.DiagnosticAudioRing.DiscontinuityKind;
import com.agoessling.swingcapture.diagnostics.DiagnosticAudioRing.Snapshot;
import com.agoessling.swingcapture.diagnostics.DiagnosticAudioRing.SnapshotAvailability;
import com.agoessling.swingcapture.diagnostics.DiagnosticAudioRing.SnapshotUnavailableException;

/** Deterministic absolute-position, wrap, discontinuity, bound, and immutability coverage. */
public final class DiagnosticAudioRingTest {
  private DiagnosticAudioRingTest() {}

  public static void main(String[] arguments) {
    wrappedExplicitWindowIsDetachedAndImmutable();
    appendLargerThanCapacityRetainsExactSuffix();
    gapsOverlapsAndOverflowAreExplicitAndNonmutating();
    snapshotBoundsAndAvailabilityAreTyped();
    recommendedCapacityIsCheckedWithoutAllocation();
  }

  private static void wrappedExplicitWindowIsDetachedAndImmutable() {
    DiagnosticAudioRing ring = new DiagnosticAudioRing(10);
    appendRange(ring, 101, 115, 4);

    check(ring.oldestRetainedFramePosition() == 105, "wrapped retained first");
    check(ring.endRetainedFramePosition() == 115, "wrapped retained end");
    Snapshot snapshot = ring.snapshot(107, 114);
    check(snapshot.firstFramePosition() == 107, "snapshot first");
    check(snapshot.endFramePosition() == 114, "snapshot exclusive end");
    check(snapshot.frameCount() == 7, "snapshot count");
    assertSamples(snapshot);

    short[] exported = new short[snapshot.frameCount()];
    snapshot.copySamplesTo(exported, 0);
    exported[0] ^= 0x7fff;
    check(snapshot.sampleAt(0) == sample(107), "export cannot mutate snapshot");
    appendRange(ring, 115, 125, 3);
    assertSamples(snapshot);
  }

  private static void appendLargerThanCapacityRetainsExactSuffix() {
    DiagnosticAudioRing ring = new DiagnosticAudioRing(5);
    short[] input = samples(1_000, 9);
    ring.append(input, 0, input.length, 1_000);
    check(ring.retainedFrameCount() == 5, "large append retained count");
    check(ring.oldestRetainedFramePosition() == 1_004, "large append retained first");
    Snapshot snapshot = ring.snapshot(1_004, 1_009);
    assertSamples(snapshot);
    input[4] ^= 0x1234;
    check(snapshot.sampleAt(0) == sample(1_004), "append copied caller input");
  }

  private static void gapsOverlapsAndOverflowAreExplicitAndNonmutating() {
    DiagnosticAudioRing ring = new DiagnosticAudioRing(8);
    ring.append(samples(50, 4), 0, 4, 50);
    AppendDiscontinuityException gap =
        expectDiscontinuity(() -> ring.append(new short[] {1}, 0, 1, 55));
    check(gap.kind() == DiscontinuityKind.GAP, "gap kind");
    check(gap.expectedFirstFramePosition() == 54, "gap expected");
    check(gap.receivedFirstFramePosition() == 55, "gap received");
    check(ring.endRetainedFramePosition() == 54, "gap is nonmutating");

    AppendDiscontinuityException overlap =
        expectDiscontinuity(() -> ring.append(new short[] {1}, 0, 1, 53));
    check(overlap.kind() == DiscontinuityKind.OVERLAP, "overlap kind");
    ring.append(new short[] {sample(54)}, 0, 1, 54);
    check(ring.nextExpectedFramePosition() == 55, "continuity resumes");

    DiagnosticAudioRing nearLimit = new DiagnosticAudioRing(4);
    nearLimit.append(new short[] {7}, 0, 1, Long.MAX_VALUE - 1);
    expectIllegalArgument(
        () -> nearLimit.append(new short[] {8}, 0, 1, Long.MAX_VALUE), "position overflow");
    check(nearLimit.endRetainedFramePosition() == Long.MAX_VALUE, "overflow is nonmutating");
  }

  private static void snapshotBoundsAndAvailabilityAreTyped() {
    DiagnosticAudioRing ring = new DiagnosticAudioRing(5);
    check(ring.snapshotAvailability(10, 11) == SnapshotAvailability.EMPTY, "empty status");
    check(
        ring.snapshotAvailability(10, 16) == SnapshotAvailability.WINDOW_EXCEEDS_CAPACITY,
        "oversize window status");
    ring.append(samples(20, 5), 0, 5, 20);
    check(
        ring.snapshotAvailability(19, 21) == SnapshotAvailability.BEFORE_RETAINED_RANGE,
        "before status");
    check(
        ring.snapshotAvailability(24, 26) == SnapshotAvailability.AFTER_RETAINED_RANGE,
        "after status");
    SnapshotUnavailableException unavailable = expectUnavailable(() -> ring.snapshot(24, 26));
    check(unavailable.availability() == SnapshotAvailability.AFTER_RETAINED_RANGE,
        "typed unavailable");
    check(unavailable.retainedFirstFramePosition() == 20, "unavailable retained first");
    check(unavailable.retainedEndFramePosition() == 25, "unavailable retained end");
    expectIllegalArgument(() -> ring.snapshotAvailability(-1, 1), "negative window");
    expectIllegalArgument(() -> ring.snapshotAvailability(2, 2), "empty window");
    expectIllegalArgument(() -> ring.append(new short[] {}, 0, 0, 25), "empty append");
    expectIllegalArgument(() -> ring.append(new short[] {1}, 0, 1, -1), "negative append");
  }

  private static void recommendedCapacityIsCheckedWithoutAllocation() {
    check(
        DiagnosticAudioRing.recommendedCapacityFrames(48_000, 60) == 2_880_000,
        "recommended frames");
    expectArithmetic(
        () -> DiagnosticAudioRing.recommendedCapacityFrames(Integer.MAX_VALUE, 2),
        "capacity multiplication overflow");
    expectIllegalArgument(
        () -> DiagnosticAudioRing.recommendedCapacityFrames(0, 60), "zero sample rate");
    expectIllegalArgument(() -> new DiagnosticAudioRing(0), "zero capacity");
  }

  private static void appendRange(
      DiagnosticAudioRing ring, long firstInclusive, long endExclusive, int blockSize) {
    long next = firstInclusive;
    while (next < endExclusive) {
      int count = (int) Math.min(blockSize, endExclusive - next);
      short[] block = samples(next, count);
      ring.append(block, 0, count, next);
      next += count;
    }
  }

  private static short[] samples(long firstPosition, int count) {
    short[] result = new short[count];
    for (int index = 0; index < count; ++index) {
      result[index] = sample(firstPosition + index);
    }
    return result;
  }

  private static short sample(long position) {
    return (short) ((position * 31 + 17) & 0xffff);
  }

  private static void assertSamples(Snapshot snapshot) {
    for (int index = 0; index < snapshot.frameCount(); ++index) {
      check(
          snapshot.sampleAt(index) == sample(snapshot.firstFramePosition() + index),
          "snapshot sample " + index);
    }
  }

  private static AppendDiscontinuityException expectDiscontinuity(Action action) {
    try {
      action.run();
    } catch (AppendDiscontinuityException expected) {
      return expected;
    }
    throw new AssertionError("expected append discontinuity");
  }

  private static SnapshotUnavailableException expectUnavailable(Action action) {
    try {
      action.run();
    } catch (SnapshotUnavailableException expected) {
      return expected;
    }
    throw new AssertionError("expected unavailable snapshot");
  }

  private static void expectIllegalArgument(Action action, String message) {
    try {
      action.run();
    } catch (IllegalArgumentException expected) {
      return;
    }
    throw new AssertionError(message + " was accepted");
  }

  private static void expectArithmetic(Action action, String message) {
    try {
      action.run();
    } catch (ArithmeticException expected) {
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
