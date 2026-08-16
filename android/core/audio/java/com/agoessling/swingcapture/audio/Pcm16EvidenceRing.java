package com.agoessling.swingcapture.audio;

import java.util.Objects;

/**
 * Fixed-capacity mono PCM16 history addressed by absolute AudioRecord frame position.
 *
 * <p>The nominal append path performs no allocation. A successful {@link #snapshot(long)} copies
 * exactly one closed interval from 1.5 seconds before the strike through 0.5 seconds after it. The
 * endpoint sample is included, so a snapshot contains {@link #SNAPSHOT_FRAME_COUNT} samples.
 */
public final class Pcm16EvidenceRing {
  public static final int SAMPLE_RATE_HZ = 48_000;
  public static final int PRE_ROLL_FRAMES = SAMPLE_RATE_HZ * 3 / 2;
  public static final int POST_ROLL_FRAMES = SAMPLE_RATE_HZ / 2;
  public static final int SNAPSHOT_FRAME_COUNT = PRE_ROLL_FRAMES + POST_ROLL_FRAMES + 1;

  public enum DiscontinuityKind {
    GAP,
    OVERLAP,
  }

  /** A rejected append that leaves all retained samples and positions unchanged. */
  public static final class AppendDiscontinuityException extends IllegalArgumentException {
    private final DiscontinuityKind kind;
    private final long expectedFirstFramePosition;
    private final long receivedFirstFramePosition;

    private AppendDiscontinuityException(
        DiscontinuityKind kind,
        long expectedFirstFramePosition,
        long receivedFirstFramePosition) {
      super(
          "PCM16 append "
              + kind.name().toLowerCase(java.util.Locale.ROOT)
              + ": expected first frame "
              + expectedFirstFramePosition
              + " but received "
              + receivedFirstFramePosition);
      this.kind = kind;
      this.expectedFirstFramePosition = expectedFirstFramePosition;
      this.receivedFirstFramePosition = receivedFirstFramePosition;
    }

    public DiscontinuityKind kind() {
      return kind;
    }

    public long expectedFirstFramePosition() {
      return expectedFirstFramePosition;
    }

    public long receivedFirstFramePosition() {
      return receivedFirstFramePosition;
    }
  }

  public enum SnapshotAvailability {
    AVAILABLE,
    EMPTY,
    STRIKE_NOT_YET_RETAINED,
    PRE_ROLL_UNAVAILABLE,
    POST_ROLL_INCOMPLETE,
  }

  /** A failed snapshot request with the retained absolute range observed atomically. */
  public static final class SnapshotUnavailableException extends IllegalStateException {
    private final SnapshotAvailability availability;
    private final long strikeFramePosition;
    private final long oldestRetainedFramePosition;
    private final long newestRetainedFramePosition;

    private SnapshotUnavailableException(
        SnapshotAvailability availability,
        long strikeFramePosition,
        long oldestRetainedFramePosition,
        long newestRetainedFramePosition) {
      super(
          "PCM16 snapshot is "
              + availability.name().toLowerCase(java.util.Locale.ROOT)
              + " for strike "
              + strikeFramePosition
              + " with retained range ["
              + oldestRetainedFramePosition
              + ", "
              + newestRetainedFramePosition
              + "]");
      this.availability = availability;
      this.strikeFramePosition = strikeFramePosition;
      this.oldestRetainedFramePosition = oldestRetainedFramePosition;
      this.newestRetainedFramePosition = newestRetainedFramePosition;
    }

    public SnapshotAvailability availability() {
      return availability;
    }

    public long strikeFramePosition() {
      return strikeFramePosition;
    }

    public long oldestRetainedFramePosition() {
      return oldestRetainedFramePosition;
    }

    public long newestRetainedFramePosition() {
      return newestRetainedFramePosition;
    }
  }

  /** Detached immutable evidence; sample data are never exposed as a mutable backing array. */
  public static final class Snapshot {
    private final long firstFramePosition;
    private final long lastFramePosition;
    private final long strikeFramePosition;
    private final short[] samples;

    private Snapshot(
        long firstFramePosition,
        long lastFramePosition,
        long strikeFramePosition,
        short[] samples) {
      this.firstFramePosition = firstFramePosition;
      this.lastFramePosition = lastFramePosition;
      this.strikeFramePosition = strikeFramePosition;
      this.samples = samples;
    }

    public long firstFramePosition() {
      return firstFramePosition;
    }

    /** Inclusive absolute position of the final copied sample. */
    public long lastFramePosition() {
      return lastFramePosition;
    }

    public long strikeFramePosition() {
      return strikeFramePosition;
    }

    public int strikeSampleIndex() {
      return PRE_ROLL_FRAMES;
    }

    public int sampleCount() {
      return samples.length;
    }

    public short sampleAt(int index) {
      Objects.checkIndex(index, samples.length);
      return samples[index];
    }

    /** Copies every sample without exposing or replacing the immutable backing evidence. */
    public void copySamplesTo(short[] destination, int offset) {
      Objects.requireNonNull(destination, "destination");
      Objects.checkFromIndexSize(offset, samples.length, destination.length);
      System.arraycopy(samples, 0, destination, offset, samples.length);
    }
  }

  private final short[] samples;
  private int retainedFrameCount;
  private long oldestFramePosition;
  private long nextExpectedFramePosition;
  private boolean initialized;

  public Pcm16EvidenceRing(int capacityFrames) {
    if (capacityFrames < SNAPSHOT_FRAME_COUNT) {
      throw new IllegalArgumentException(
          "PCM16 capacity must retain at least " + SNAPSHOT_FRAME_COUNT + " frames");
    }
    samples = new short[capacityFrames];
  }

  public int capacityFrames() {
    return samples.length;
  }

  /**
   * Copies one nonempty contiguous AudioRecord block into the ring without allocating.
   *
   * <p>The first successful append establishes the absolute position. Every later append must
   * begin at the preceding block's exclusive end. The append exclusive end must fit in a signed
   * {@code long}, leaving a representable expected position for the next block.
   */
  public synchronized void append(
      short[] pcm, int offset, int frameCount, long firstFramePosition) {
    Objects.requireNonNull(pcm, "pcm");
    Objects.checkFromIndexSize(offset, frameCount, pcm.length);
    if (frameCount <= 0) {
      throw new IllegalArgumentException("PCM16 append must contain at least one frame");
    }
    if (firstFramePosition < 0) {
      throw new IllegalArgumentException("first frame position must be nonnegative");
    }
    if (firstFramePosition > Long.MAX_VALUE - frameCount) {
      throw new IllegalArgumentException("PCM16 append exclusive frame position overflows long");
    }
    if (initialized && firstFramePosition != nextExpectedFramePosition) {
      DiscontinuityKind kind =
          firstFramePosition > nextExpectedFramePosition
              ? DiscontinuityKind.GAP
              : DiscontinuityKind.OVERLAP;
      throw new AppendDiscontinuityException(
          kind, nextExpectedFramePosition, firstFramePosition);
    }

    long exclusiveEnd = firstFramePosition + frameCount;
    int copiedFrames = Math.min(frameCount, samples.length);
    int source = offset + frameCount - copiedFrames;
    long copiedFirstFrame = exclusiveEnd - copiedFrames;
    int destination = (int) (copiedFirstFrame % samples.length);
    int firstCopy = Math.min(copiedFrames, samples.length - destination);
    System.arraycopy(pcm, source, samples, destination, firstCopy);
    if (firstCopy < copiedFrames) {
      System.arraycopy(pcm, source + firstCopy, samples, 0, copiedFrames - firstCopy);
    }

    if (frameCount >= samples.length - retainedFrameCount) {
      retainedFrameCount = samples.length;
    } else {
      retainedFrameCount += frameCount;
    }
    oldestFramePosition = exclusiveEnd - retainedFrameCount;
    nextExpectedFramePosition = exclusiveEnd;
    initialized = true;
  }

  /** Allocation-free readiness check for one strike position. */
  public synchronized SnapshotAvailability snapshotAvailability(long strikeFramePosition) {
    validateStrike(strikeFramePosition);
    if (!initialized) {
      return SnapshotAvailability.EMPTY;
    }
    if (strikeFramePosition >= nextExpectedFramePosition) {
      return SnapshotAvailability.STRIKE_NOT_YET_RETAINED;
    }
    long first = strikeFramePosition - PRE_ROLL_FRAMES;
    if (first < 0 || first < oldestFramePosition) {
      return SnapshotAvailability.PRE_ROLL_UNAVAILABLE;
    }
    long last = snapshotLastFrame(strikeFramePosition);
    if (last >= nextExpectedFramePosition) {
      return SnapshotAvailability.POST_ROLL_INCOMPLETE;
    }
    return SnapshotAvailability.AVAILABLE;
  }

  /** Allocates and returns a detached copy only when the complete closed window is retained. */
  public synchronized Snapshot snapshot(long strikeFramePosition) {
    SnapshotAvailability availability = snapshotAvailability(strikeFramePosition);
    if (availability != SnapshotAvailability.AVAILABLE) {
      throw new SnapshotUnavailableException(
          availability,
          strikeFramePosition,
          initialized ? oldestFramePosition : -1,
          initialized ? nextExpectedFramePosition - 1 : -1);
    }
    long first = strikeFramePosition - PRE_ROLL_FRAMES;
    long last = snapshotLastFrame(strikeFramePosition);
    short[] copy = new short[SNAPSHOT_FRAME_COUNT];
    int source = (int) (first % samples.length);
    int firstCopy = Math.min(copy.length, samples.length - source);
    System.arraycopy(samples, source, copy, 0, firstCopy);
    if (firstCopy < copy.length) {
      System.arraycopy(samples, 0, copy, firstCopy, copy.length - firstCopy);
    }
    return new Snapshot(first, last, strikeFramePosition, copy);
  }

  public synchronized int retainedFrameCount() {
    return retainedFrameCount;
  }

  public synchronized long oldestRetainedFramePosition() {
    return initialized ? oldestFramePosition : -1;
  }

  public synchronized long newestRetainedFramePosition() {
    return initialized ? nextExpectedFramePosition - 1 : -1;
  }

  public synchronized long nextExpectedFramePosition() {
    return initialized ? nextExpectedFramePosition : -1;
  }

  private static void validateStrike(long strikeFramePosition) {
    if (strikeFramePosition < 0) {
      throw new IllegalArgumentException("strike frame position must be nonnegative");
    }
    snapshotLastFrame(strikeFramePosition);
  }

  private static long snapshotLastFrame(long strikeFramePosition) {
    if (strikeFramePosition > Long.MAX_VALUE - POST_ROLL_FRAMES) {
      throw new IllegalArgumentException("snapshot frame range overflows long");
    }
    return strikeFramePosition + POST_ROLL_FRAMES;
  }
}
