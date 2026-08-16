package com.agoessling.swingcapture.audio;

import com.agoessling.swingcapture.audio.Pcm16EvidenceRing.AppendDiscontinuityException;
import com.agoessling.swingcapture.audio.Pcm16EvidenceRing.DiscontinuityKind;
import com.agoessling.swingcapture.audio.Pcm16EvidenceRing.Snapshot;
import com.agoessling.swingcapture.audio.Pcm16EvidenceRing.SnapshotAvailability;
import com.agoessling.swingcapture.audio.Pcm16EvidenceRing.SnapshotUnavailableException;

/** Deterministic absolute-position, wrap, boundary, and failure coverage. */
public final class Pcm16EvidenceRingTest {
  private Pcm16EvidenceRingTest() {}

  public static void main(String[] arguments) {
    wrappedHistoryProducesDetachedImmutableSnapshot();
    delayedTriggerCopiesAlreadyCompleteWindow();
    incompletePostRollBecomesAvailableAtEndpoint();
    gapsOverlapsAndOverflowAreExplicitAndNonmutating();
    exactCapacityWindowExpiresAfterOneMoreFrame();
  }

  private static void wrappedHistoryProducesDetachedImmutableSnapshot() {
    int capacity = Pcm16EvidenceRing.SNAPSHOT_FRAME_COUNT + 16;
    Pcm16EvidenceRing ring = new Pcm16EvidenceRing(capacity);
    long streamFirst = 1_000_003;
    long snapshotFirst = streamFirst + 40;
    long strike = snapshotFirst + Pcm16EvidenceRing.PRE_ROLL_FRAMES;
    long snapshotLast = strike + Pcm16EvidenceRing.POST_ROLL_FRAMES;

    appendRange(ring, streamFirst, snapshotLast + 1, 997);
    check(ring.oldestRetainedFramePosition() == streamFirst + 24, "wrapped oldest position");
    check(ring.newestRetainedFramePosition() == snapshotLast, "wrapped newest position");
    Snapshot snapshot = ring.snapshot(strike);
    check(snapshot.firstFramePosition() == snapshotFirst, "snapshot first position");
    check(snapshot.lastFramePosition() == snapshotLast, "snapshot last position");
    check(snapshot.strikeFramePosition() == strike, "snapshot strike position");
    check(snapshot.strikeSampleIndex() == Pcm16EvidenceRing.PRE_ROLL_FRAMES,
        "strike index");
    check(snapshot.sampleCount() == Pcm16EvidenceRing.SNAPSHOT_FRAME_COUNT,
        "closed-window sample count");
    assertSnapshotContents(snapshot);

    short[] exported = new short[snapshot.sampleCount()];
    snapshot.copySamplesTo(exported, 0);
    exported[0] ^= 0x7fff;
    check(snapshot.sampleAt(0) == sample(snapshotFirst), "export cannot mutate snapshot");

    appendRange(ring, snapshotLast + 1, snapshotLast + capacity + 2L, 4_000);
    assertSnapshotContents(snapshot);
  }

  private static void delayedTriggerCopiesAlreadyCompleteWindow() {
    Pcm16EvidenceRing ring =
        new Pcm16EvidenceRing(Pcm16EvidenceRing.SNAPSHOT_FRAME_COUNT + 1_000);
    long first = 500_000;
    long strike = first + Pcm16EvidenceRing.PRE_ROLL_FRAMES + 100;
    long last = strike + Pcm16EvidenceRing.POST_ROLL_FRAMES + 300;
    appendRange(ring, first, last + 1, 2_048);

    check(ring.snapshotAvailability(strike) == SnapshotAvailability.AVAILABLE,
        "delayed trigger remains available");
    Snapshot snapshot = ring.snapshot(strike);
    check(snapshot.firstFramePosition() == strike - Pcm16EvidenceRing.PRE_ROLL_FRAMES,
        "delayed first position");
    check(snapshot.lastFramePosition() == strike + Pcm16EvidenceRing.POST_ROLL_FRAMES,
        "delayed last position");
    assertSnapshotContents(snapshot);
  }

  private static void incompletePostRollBecomesAvailableAtEndpoint() {
    Pcm16EvidenceRing ring =
        new Pcm16EvidenceRing(Pcm16EvidenceRing.SNAPSHOT_FRAME_COUNT + 32);
    long first = 80_000;
    long strike = first + Pcm16EvidenceRing.PRE_ROLL_FRAMES;
    long requiredLast = strike + Pcm16EvidenceRing.POST_ROLL_FRAMES;
    appendRange(ring, first, requiredLast, 1_024);

    check(ring.snapshotAvailability(strike) == SnapshotAvailability.POST_ROLL_INCOMPLETE,
        "endpoint sample is required");
    SnapshotUnavailableException incomplete = expectUnavailable(() -> ring.snapshot(strike));
    check(incomplete.availability() == SnapshotAvailability.POST_ROLL_INCOMPLETE,
        "incomplete status");
    check(incomplete.strikeFramePosition() == strike, "incomplete strike evidence");
    check(incomplete.newestRetainedFramePosition() == requiredLast - 1,
        "incomplete newest evidence");

    short[] endpoint = {sample(requiredLast)};
    ring.append(endpoint, 0, endpoint.length, requiredLast);
    check(ring.snapshotAvailability(strike) == SnapshotAvailability.AVAILABLE,
        "endpoint completes post-roll");
    check(ring.snapshot(strike).sampleAt(Pcm16EvidenceRing.SNAPSHOT_FRAME_COUNT - 1)
            == sample(requiredLast),
        "endpoint copied");
  }

  private static void gapsOverlapsAndOverflowAreExplicitAndNonmutating() {
    Pcm16EvidenceRing ring = new Pcm16EvidenceRing(Pcm16EvidenceRing.SNAPSHOT_FRAME_COUNT);
    short[] initial = samples(100, 10);
    ring.append(initial, 0, initial.length, 100);

    AppendDiscontinuityException gap =
        expectDiscontinuity(() -> ring.append(new short[] {1}, 0, 1, 111));
    check(gap.kind() == DiscontinuityKind.GAP, "gap kind");
    check(gap.expectedFirstFramePosition() == 110, "gap expected position");
    check(gap.receivedFirstFramePosition() == 111, "gap received position");
    check(ring.newestRetainedFramePosition() == 109, "gap does not mutate history");

    AppendDiscontinuityException overlap =
        expectDiscontinuity(() -> ring.append(new short[] {1}, 0, 1, 109));
    check(overlap.kind() == DiscontinuityKind.OVERLAP, "overlap kind");
    check(overlap.expectedFirstFramePosition() == 110, "overlap expected position");
    check(overlap.receivedFirstFramePosition() == 109, "overlap received position");
    ring.append(new short[] {sample(110)}, 0, 1, 110);
    check(ring.newestRetainedFramePosition() == 110, "continuity resumes after rejection");

    Pcm16EvidenceRing nearLimit =
        new Pcm16EvidenceRing(Pcm16EvidenceRing.SNAPSHOT_FRAME_COUNT);
    short[] finalRepresentable = {7, 8};
    nearLimit.append(finalRepresentable, 0, 2, Long.MAX_VALUE - 2);
    expectIllegalArgument(
        () -> nearLimit.append(new short[] {9}, 0, 1, Long.MAX_VALUE),
        "exclusive end overflow");
    check(nearLimit.newestRetainedFramePosition() == Long.MAX_VALUE - 1,
        "overflow does not mutate history");
    expectIllegalArgument(
        () -> ring.snapshotAvailability(Long.MAX_VALUE), "snapshot range overflow");
  }

  private static void exactCapacityWindowExpiresAfterOneMoreFrame() {
    Pcm16EvidenceRing ring = new Pcm16EvidenceRing(Pcm16EvidenceRing.SNAPSHOT_FRAME_COUNT);
    long first = 200_000;
    long strike = first + Pcm16EvidenceRing.PRE_ROLL_FRAMES;
    long last = strike + Pcm16EvidenceRing.POST_ROLL_FRAMES;
    appendRange(ring, first, last + 1, 3_000);

    check(ring.retainedFrameCount() == Pcm16EvidenceRing.SNAPSHOT_FRAME_COUNT,
        "exact capacity retained");
    check(ring.snapshotAvailability(strike) == SnapshotAvailability.AVAILABLE,
        "exact closed bound available");
    Snapshot snapshot = ring.snapshot(strike);
    check(snapshot.sampleAt(0) == sample(first), "exact first sample");
    check(snapshot.sampleAt(snapshot.sampleCount() - 1) == sample(last),
        "exact last sample");

    ring.append(new short[] {sample(last + 1)}, 0, 1, last + 1);
    check(ring.snapshotAvailability(strike) == SnapshotAvailability.PRE_ROLL_UNAVAILABLE,
        "one additional frame expires exact-capacity pre-roll");
    check(snapshot.sampleAt(0) == sample(first), "completed snapshot remains detached");
  }

  private static void appendRange(
      Pcm16EvidenceRing ring, long firstInclusive, long endExclusive, int blockFrames) {
    long next = firstInclusive;
    while (next < endExclusive) {
      int count = (int) Math.min(blockFrames, endExclusive - next);
      short[] block = samples(next, count);
      ring.append(block, 0, block.length, next);
      for (int index = 0; index < block.length; ++index) {
        block[index] ^= 0x55aa;
      }
      next += count;
    }
  }

  private static short[] samples(long firstFramePosition, int count) {
    short[] result = new short[count];
    for (int index = 0; index < count; ++index) {
      result[index] = sample(firstFramePosition + index);
    }
    return result;
  }

  private static short sample(long framePosition) {
    return (short) ((framePosition * 31 + 17) & 0xffff);
  }

  private static void assertSnapshotContents(Snapshot snapshot) {
    for (int index = 0; index < snapshot.sampleCount(); ++index) {
      check(snapshot.sampleAt(index) == sample(snapshot.firstFramePosition() + index),
          "snapshot sample " + index);
    }
  }

  private static AppendDiscontinuityException expectDiscontinuity(Action action) {
    try {
      action.run();
    } catch (AppendDiscontinuityException expected) {
      return expected;
    }
    throw new AssertionError("expected PCM16 append discontinuity");
  }

  private static SnapshotUnavailableException expectUnavailable(Action action) {
    try {
      action.run();
    } catch (SnapshotUnavailableException expected) {
      return expected;
    }
    throw new AssertionError("expected unavailable PCM16 snapshot");
  }

  private static void expectIllegalArgument(Action action, String message) {
    try {
      action.run();
    } catch (IllegalArgumentException expected) {
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
