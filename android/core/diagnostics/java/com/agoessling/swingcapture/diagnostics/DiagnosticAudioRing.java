package com.agoessling.swingcapture.diagnostics;

import java.util.Locale;
import java.util.Objects;

/**
 * Fixed-capacity mono PCM16 flight recorder addressed by absolute AudioRecord frame position.
 *
 * <p>A recommended field configuration is 48 kHz with 60 seconds of history: construct with
 * {@code new DiagnosticAudioRing(recommendedCapacityFrames(48_000, 60))}. Constants describe that
 * policy, but no buffer is allocated until an instance is constructed.
 */
public final class DiagnosticAudioRing {
  public static final int RECOMMENDED_SAMPLE_RATE_HZ = 48_000;
  public static final int RECOMMENDED_RETENTION_SECONDS = 60;

  public enum DiscontinuityKind {
    GAP,
    OVERLAP,
  }

  /** A rejected append; the ring is unchanged and may resume at the expected position. */
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
              + kind.name().toLowerCase(Locale.ROOT)
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
    BEFORE_RETAINED_RANGE,
    AFTER_RETAINED_RANGE,
    WINDOW_EXCEEDS_CAPACITY,
  }

  /** A rejected snapshot with the requested and currently retained half-open ranges. */
  public static final class SnapshotUnavailableException extends IllegalStateException {
    private final SnapshotAvailability availability;
    private final long requestedFirstFramePosition;
    private final long requestedEndFramePosition;
    private final long retainedFirstFramePosition;
    private final long retainedEndFramePosition;

    private SnapshotUnavailableException(
        SnapshotAvailability availability,
        long requestedFirstFramePosition,
        long requestedEndFramePosition,
        long retainedFirstFramePosition,
        long retainedEndFramePosition) {
      super(
          "PCM16 snapshot is "
              + availability.name().toLowerCase(Locale.ROOT)
              + " for requested range ["
              + requestedFirstFramePosition
              + ", "
              + requestedEndFramePosition
              + ") with retained range ["
              + retainedFirstFramePosition
              + ", "
              + retainedEndFramePosition
              + ")");
      this.availability = availability;
      this.requestedFirstFramePosition = requestedFirstFramePosition;
      this.requestedEndFramePosition = requestedEndFramePosition;
      this.retainedFirstFramePosition = retainedFirstFramePosition;
      this.retainedEndFramePosition = retainedEndFramePosition;
    }

    public SnapshotAvailability availability() {
      return availability;
    }

    public long requestedFirstFramePosition() {
      return requestedFirstFramePosition;
    }

    public long requestedEndFramePosition() {
      return requestedEndFramePosition;
    }

    public long retainedFirstFramePosition() {
      return retainedFirstFramePosition;
    }

    public long retainedEndFramePosition() {
      return retainedEndFramePosition;
    }
  }

  /** Detached immutable PCM16 evidence for one explicit half-open absolute-frame window. */
  public static final class Snapshot {
    private final long firstFramePosition;
    private final long endFramePosition;
    private final short[] samples;

    private Snapshot(long firstFramePosition, long endFramePosition, short[] samples) {
      this.firstFramePosition = firstFramePosition;
      this.endFramePosition = endFramePosition;
      this.samples = samples;
    }

    public long firstFramePosition() {
      return firstFramePosition;
    }

    public long endFramePosition() {
      return endFramePosition;
    }

    public int frameCount() {
      return samples.length;
    }

    public short sampleAt(int index) {
      Objects.checkIndex(index, samples.length);
      return samples[index];
    }

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

  public DiagnosticAudioRing(int capacityFrames) {
    if (capacityFrames <= 0) {
      throw new IllegalArgumentException("PCM16 capacity must be positive");
    }
    samples = new short[capacityFrames];
  }

  public static int recommendedCapacityFrames(int sampleRateHz, int retentionSeconds) {
    if (sampleRateHz <= 0 || retentionSeconds <= 0) {
      throw new IllegalArgumentException("sample rate and retention must be positive");
    }
    return Math.multiplyExact(sampleRateHz, retentionSeconds);
  }

  public int capacityFrames() {
    return samples.length;
  }

  /** Copies one contiguous nonempty block without allocation on the successful append path. */
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

    long endFramePosition = firstFramePosition + frameCount;
    int copiedFrames = Math.min(frameCount, samples.length);
    int source = offset + frameCount - copiedFrames;
    long copiedFirstFramePosition = endFramePosition - copiedFrames;
    int destination = (int) (copiedFirstFramePosition % samples.length);
    int firstCopy = Math.min(copiedFrames, samples.length - destination);
    System.arraycopy(pcm, source, samples, destination, firstCopy);
    if (firstCopy < copiedFrames) {
      System.arraycopy(pcm, source + firstCopy, samples, 0, copiedFrames - firstCopy);
    }

    retainedFrameCount = Math.min(samples.length, retainedFrameCount + copiedFrames);
    oldestFramePosition = endFramePosition - retainedFrameCount;
    nextExpectedFramePosition = endFramePosition;
    initialized = true;
  }

  public synchronized SnapshotAvailability snapshotAvailability(
      long firstFramePosition, long endFramePosition) {
    validateWindow(firstFramePosition, endFramePosition);
    long frameCount = endFramePosition - firstFramePosition;
    if (frameCount > samples.length) {
      return SnapshotAvailability.WINDOW_EXCEEDS_CAPACITY;
    }
    if (!initialized) {
      return SnapshotAvailability.EMPTY;
    }
    if (firstFramePosition < oldestFramePosition) {
      return SnapshotAvailability.BEFORE_RETAINED_RANGE;
    }
    if (endFramePosition > nextExpectedFramePosition) {
      return SnapshotAvailability.AFTER_RETAINED_RANGE;
    }
    return SnapshotAvailability.AVAILABLE;
  }

  /** Returns a detached copy of exactly {@code [firstFramePosition, endFramePosition)}. */
  public synchronized Snapshot snapshot(long firstFramePosition, long endFramePosition) {
    SnapshotAvailability availability =
        snapshotAvailability(firstFramePosition, endFramePosition);
    if (availability != SnapshotAvailability.AVAILABLE) {
      throw new SnapshotUnavailableException(
          availability,
          firstFramePosition,
          endFramePosition,
          initialized ? oldestFramePosition : -1,
          initialized ? nextExpectedFramePosition : -1);
    }
    int frameCount = Math.toIntExact(endFramePosition - firstFramePosition);
    short[] copy = new short[frameCount];
    int source = (int) (firstFramePosition % samples.length);
    int firstCopy = Math.min(frameCount, samples.length - source);
    System.arraycopy(samples, source, copy, 0, firstCopy);
    if (firstCopy < frameCount) {
      System.arraycopy(samples, 0, copy, firstCopy, frameCount - firstCopy);
    }
    return new Snapshot(firstFramePosition, endFramePosition, copy);
  }

  public synchronized int retainedFrameCount() {
    return retainedFrameCount;
  }

  public synchronized long oldestRetainedFramePosition() {
    return initialized ? oldestFramePosition : -1;
  }

  public synchronized long endRetainedFramePosition() {
    return initialized ? nextExpectedFramePosition : -1;
  }

  public synchronized long nextExpectedFramePosition() {
    return initialized ? nextExpectedFramePosition : -1;
  }

  private static void validateWindow(long firstFramePosition, long endFramePosition) {
    if (firstFramePosition < 0) {
      throw new IllegalArgumentException("snapshot first frame position must be nonnegative");
    }
    if (endFramePosition <= firstFramePosition) {
      throw new IllegalArgumentException("snapshot range must be nonempty and increasing");
    }
  }
}
