package com.agoessling.swingcapture;

import com.agoessling.swingcapture.diagnostics.PreviewEvidence;
import java.util.Objects;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadFactory;

/**
 * Bounded, latest-frame-wins setup preview pipeline fed by detached low-rate camera data.
 *
 * <p>This class never opens, closes, or retains a Camera2 {@code Image}. The camera owner may offer
 * a detached NV21 copy after inference has finished, or reuse an already-encoded {@link
 * PreviewEvidence} frame. At most one encode and one latest pending input are retained. The
 * encoder runs serially, and published JPEGs have explicit availability and staleness states.
 */
final class SetupPreviewProvider implements AutoCloseable {
  static final long DEFAULT_MINIMUM_FRAME_INTERVAL_NANOS = 1_000_000_000L;
  static final long DEFAULT_MAXIMUM_FRAME_AGE_NANOS = 3_000_000_000L;
  static final int DEFAULT_MAXIMUM_INPUT_BYTES = 2 * 1024 * 1024;
  static final int DEFAULT_MAXIMUM_JPEG_BYTES = 512 * 1024;
  static final int DEFAULT_JPEG_QUALITY = 65;

  enum State {
    AVAILABLE,
    STALE,
    UNAVAILABLE
  }

  enum Reason {
    NONE(false),
    STARTING(true),
    NO_FRAME(false),
    SOURCE_PAUSED(true),
    HIGH_SPEED_CAPTURE(true),
    POSE_DISABLED(true),
    SOURCE_ERROR(true),
    ENCODING_FAILED(false),
    TIMESTAMP_IN_FUTURE(false),
    STALE(false),
    STOPPED(true);

    private final boolean sourceControlled;

    Reason(boolean sourceControlled) {
      this.sourceControlled = sourceControlled;
    }

    boolean sourceControlled() {
      return sourceControlled;
    }
  }

  enum OfferDisposition {
    SCHEDULED,
    QUEUED_LATEST,
    REPLACED_PENDING,
    PUBLISHED_ENCODED,
    SKIPPED_CADENCE,
    REJECTED_SOURCE_UNAVAILABLE,
    REJECTED_STOPPED,
    REJECTED_NO_ENCODED_FRAME,
    REJECTED_INVALID_ENCODED_FRAME
  }

  record Config(
      long minimumFrameIntervalNanos,
      long maximumFrameAgeNanos,
      int maximumInputBytes,
      int maximumJpegBytes,
      int jpegQuality) {
    Config {
      if (minimumFrameIntervalNanos <= 0
          || maximumFrameAgeNanos <= 0
          || maximumInputBytes <= 0
          || maximumJpegBytes <= 0
          || jpegQuality < 1
          || jpegQuality > 100) {
        throw new IllegalArgumentException("setup preview bounds and JPEG quality are invalid");
      }
    }

    static Config defaults() {
      return new Config(
          DEFAULT_MINIMUM_FRAME_INTERVAL_NANOS,
          DEFAULT_MAXIMUM_FRAME_AGE_NANOS,
          DEFAULT_MAXIMUM_INPUT_BYTES,
          DEFAULT_MAXIMUM_JPEG_BYTES,
          DEFAULT_JPEG_QUALITY);
    }
  }

  /** Encoder input is provider-owned and detached from Camera2 before this call. */
  @FunctionalInterface
  interface Encoder {
    byte[] encodeNv21(
        byte[] nv21,
        int width,
        int height,
        int jpegQuality,
        int maximumJpegBytes);
  }

  record OfferResult(OfferDisposition disposition, long generation) {
    OfferResult {
      Objects.requireNonNull(disposition, "disposition");
      if (generation < 0) {
        throw new IllegalArgumentException("generation cannot be negative");
      }
    }

    boolean accepted() {
      return disposition == OfferDisposition.SCHEDULED
          || disposition == OfferDisposition.QUEUED_LATEST
          || disposition == OfferDisposition.REPLACED_PENDING
          || disposition == OfferDisposition.PUBLISHED_ENCODED;
    }
  }

  record Metrics(
      long offeredFrames,
      long cadenceSkippedFrames,
      long replacedPendingFrames,
      long encodingAttempts,
      long encodingFailures,
      long publishedFrames,
      int retainedWorkCount) {
    Metrics {
      if (offeredFrames < 0
          || cadenceSkippedFrames < 0
          || replacedPendingFrames < 0
          || encodingAttempts < 0
          || encodingFailures < 0
          || publishedFrames < 0
          || retainedWorkCount < 0
          || retainedWorkCount > 2) {
        throw new IllegalArgumentException("setup preview metrics are invalid");
      }
    }
  }

  record Snapshot(
      State state,
      Reason reason,
      long generation,
      long frameTimestampBoottimeNanos,
      long frameAgeNanos,
      byte[] jpeg,
      Metrics metrics) {
    Snapshot {
      Objects.requireNonNull(state, "state");
      Objects.requireNonNull(reason, "reason");
      Objects.requireNonNull(jpeg, "jpeg");
      Objects.requireNonNull(metrics, "metrics");
      if (generation < 0 || frameTimestampBoottimeNanos < -1 || frameAgeNanos < -1) {
        throw new IllegalArgumentException("setup preview snapshot timing is invalid");
      }
      if (state == State.AVAILABLE && reason != Reason.NONE) {
        throw new IllegalArgumentException("available setup preview cannot have an error reason");
      }
      if (state == State.STALE && reason != Reason.STALE) {
        throw new IllegalArgumentException("stale setup preview requires the stale reason");
      }
      if ((state == State.AVAILABLE || state == State.STALE) != (jpeg.length > 0)) {
        throw new IllegalArgumentException("setup preview state and JPEG presence disagree");
      }
      jpeg = jpeg.clone();
    }

    @Override
    public byte[] jpeg() {
      return jpeg.clone();
    }

    boolean hasFrame() {
      return jpeg.length > 0;
    }
  }

  private record Work(
      long sourceGeneration, long timestampBoottimeNanos, int width, int height, byte[] nv21) {}

  private final Object stateLock = new Object();
  private final Config config;
  private final Encoder encoder;
  private final Executor executor;
  private final ExecutorService ownedExecutor;
  private final LatestFrameSlot<Work> workSlot = new LatestFrameSlot<>();

  private boolean closed;
  private Reason sourceReason = Reason.STARTING;
  private Reason noFrameReason = Reason.NO_FRAME;
  private long sourceGeneration;
  private long lastSeenTimestampNanos = -1;
  private long lastAcceptedTimestampNanos = -1;
  private long publishedTimestampNanos = -1;
  private byte[] publishedJpeg = new byte[0];
  private long offeredFrames;
  private long cadenceSkippedFrames;
  private long replacedPendingFrames;
  private long encodingAttempts;
  private long encodingFailures;
  private long publishedFrames;

  static SetupPreviewProvider create(Config config, Encoder encoder) {
    ExecutorService executor =
        Executors.newSingleThreadExecutor(new PreviewThreadFactory());
    return new SetupPreviewProvider(config, encoder, executor, executor);
  }

  SetupPreviewProvider(Config config, Encoder encoder, Executor executor) {
    this(config, encoder, executor, null);
  }

  private SetupPreviewProvider(
      Config config, Encoder encoder, Executor executor, ExecutorService ownedExecutor) {
    this.config = Objects.requireNonNull(config, "config");
    this.encoder = Objects.requireNonNull(encoder, "encoder");
    this.executor = Objects.requireNonNull(executor, "executor");
    this.ownedExecutor = ownedExecutor;
  }

  /** Marks a camera-owned low-rate source ready; no frame is claimed until one is published. */
  void markSourceAvailable() {
    synchronized (stateLock) {
      if (closed) {
        throw new IllegalStateException("setup preview provider is stopped");
      }
      sourceGeneration = increment(sourceGeneration);
      sourceReason = Reason.NONE;
      noFrameReason = Reason.NO_FRAME;
      lastSeenTimestampNanos = -1;
      lastAcceptedTimestampNanos = -1;
      publishedTimestampNanos = -1;
      publishedJpeg = new byte[0];
    }
  }

  /**
   * Stops accepting source frames without touching camera ownership.
   *
   * <p>Already-detached encode work may finish, but its old generation can no longer publish.
   */
  void markSourceUnavailable(Reason reason) {
    Objects.requireNonNull(reason, "reason");
    if (!reason.sourceControlled() || reason == Reason.STOPPED) {
      throw new IllegalArgumentException("reason is not a live source-unavailable state");
    }
    synchronized (stateLock) {
      if (closed) {
        return;
      }
      sourceGeneration = increment(sourceGeneration);
      sourceReason = reason;
      noFrameReason = Reason.NO_FRAME;
      lastSeenTimestampNanos = -1;
      lastAcceptedTimestampNanos = -1;
      publishedTimestampNanos = -1;
      publishedJpeg = new byte[0];
    }
  }

  /** Offers one detached NV21 frame; cadence is checked before any input copy is made. */
  OfferResult offerNv21(
      long timestampBoottimeNanos, int width, int height, byte[] nv21) {
    Objects.requireNonNull(nv21, "nv21");
    int expectedBytes = exactNv21Bytes(width, height);
    if (timestampBoottimeNanos < 0) {
      throw new IllegalArgumentException("setup preview timestamp must be nonnegative");
    }
    if (expectedBytes > config.maximumInputBytes() || nv21.length != expectedBytes) {
      throw new IllegalArgumentException("setup preview NV21 input violates its byte bound");
    }

    final long generation;
    synchronized (stateLock) {
      offeredFrames = increment(offeredFrames);
      if (closed) {
        return new OfferResult(OfferDisposition.REJECTED_STOPPED, sourceGeneration);
      }
      if (sourceReason != Reason.NONE) {
        return new OfferResult(
            OfferDisposition.REJECTED_SOURCE_UNAVAILABLE, sourceGeneration);
      }
      if (lastSeenTimestampNanos >= 0
          && timestampBoottimeNanos <= lastSeenTimestampNanos) {
        throw new IllegalArgumentException("setup preview source timestamps must increase");
      }
      lastSeenTimestampNanos = timestampBoottimeNanos;
      if (lastAcceptedTimestampNanos >= 0) {
        if (timestampBoottimeNanos - lastAcceptedTimestampNanos
            < config.minimumFrameIntervalNanos()) {
          cadenceSkippedFrames = increment(cadenceSkippedFrames);
          return new OfferResult(OfferDisposition.SKIPPED_CADENCE, sourceGeneration);
        }
      }
      lastAcceptedTimestampNanos = timestampBoottimeNanos;
      generation = sourceGeneration;
    }

    Work work =
        new Work(generation, timestampBoottimeNanos, width, height, nv21.clone());
    LatestFrameSlot.Offer<Work> offer = workSlot.offer(work);
    if (!offer.accepted()) {
      synchronized (stateLock) {
        return new OfferResult(
            closed ? OfferDisposition.REJECTED_STOPPED
                : OfferDisposition.REJECTED_SOURCE_UNAVAILABLE,
            sourceGeneration);
      }
    }
    if (offer.dropped() != null) {
      synchronized (stateLock) {
        replacedPendingFrames = increment(replacedPendingFrames);
      }
    }
    if (offer.scheduleNow() != null) {
      scheduleDrain(offer.scheduleNow());
      return new OfferResult(OfferDisposition.SCHEDULED, generation);
    }
    return new OfferResult(
        offer.dropped() == null
            ? OfferDisposition.QUEUED_LATEST
            : OfferDisposition.REPLACED_PENDING,
        generation);
  }

  /** Reuses a lower-cadence JPEG already detached into the diagnostic evidence ring. */
  OfferResult offerEvidence(PreviewEvidence evidence) {
    Objects.requireNonNull(evidence, "evidence");
    synchronized (stateLock) {
      offeredFrames = increment(offeredFrames);
      if (closed) {
        return new OfferResult(OfferDisposition.REJECTED_STOPPED, sourceGeneration);
      }
      if (sourceReason != Reason.NONE) {
        return new OfferResult(
            OfferDisposition.REJECTED_SOURCE_UNAVAILABLE, sourceGeneration);
      }
      if (!evidence.hasCompressedFrame()) {
        return new OfferResult(
            OfferDisposition.REJECTED_NO_ENCODED_FRAME, sourceGeneration);
      }
      if (evidence.compressedFrameBytes() > config.maximumJpegBytes()) {
        encodingFailures = increment(encodingFailures);
        noFrameReason = Reason.ENCODING_FAILED;
        return new OfferResult(
            OfferDisposition.REJECTED_INVALID_ENCODED_FRAME, sourceGeneration);
      }
      long timestamp = evidence.timestampBoottimeNanos();
      if (lastSeenTimestampNanos >= 0 && timestamp <= lastSeenTimestampNanos) {
        throw new IllegalArgumentException("setup preview source timestamps must increase");
      }
      lastSeenTimestampNanos = timestamp;
      if (!cadenceAllowsLocked(timestamp)) {
        cadenceSkippedFrames = increment(cadenceSkippedFrames);
        return new OfferResult(OfferDisposition.SKIPPED_CADENCE, sourceGeneration);
      }
      byte[] jpeg = evidence.copyCompressedFrame();
      if (!validJpeg(jpeg) || jpeg.length > config.maximumJpegBytes()) {
        encodingFailures = increment(encodingFailures);
        noFrameReason = Reason.ENCODING_FAILED;
        return new OfferResult(
            OfferDisposition.REJECTED_INVALID_ENCODED_FRAME, sourceGeneration);
      }
      lastAcceptedTimestampNanos = timestamp;
      publishLocked(sourceGeneration, timestamp, jpeg);
      return new OfferResult(OfferDisposition.PUBLISHED_ENCODED, sourceGeneration);
    }
  }

  Snapshot snapshot(long nowBoottimeNanos) {
    if (nowBoottimeNanos < 0) {
      throw new IllegalArgumentException("setup preview snapshot time must be nonnegative");
    }
    synchronized (stateLock) {
      Metrics metrics = metricsLocked();
      if (closed || sourceReason != Reason.NONE) {
        return unavailableSnapshot(closed ? Reason.STOPPED : sourceReason, metrics);
      }
      if (publishedJpeg.length == 0) {
        return unavailableSnapshot(noFrameReason, metrics);
      }
      if (nowBoottimeNanos < publishedTimestampNanos) {
        return unavailableSnapshot(Reason.TIMESTAMP_IN_FUTURE, metrics);
      }
      long age = nowBoottimeNanos - publishedTimestampNanos;
      State state = age > config.maximumFrameAgeNanos() ? State.STALE : State.AVAILABLE;
      return new Snapshot(
          state,
          state == State.STALE ? Reason.STALE : Reason.NONE,
          sourceGeneration,
          publishedTimestampNanos,
          age,
          publishedJpeg,
          metrics);
    }
  }

  @Override
  public void close() {
    synchronized (stateLock) {
      if (closed) {
        return;
      }
      closed = true;
      sourceGeneration = increment(sourceGeneration);
      sourceReason = Reason.STOPPED;
      publishedTimestampNanos = -1;
      publishedJpeg = new byte[0];
    }
    workSlot.stopAcceptingAndTakePending();
    if (ownedExecutor != null) {
      ownedExecutor.shutdownNow();
    }
  }

  private void scheduleDrain(Work first) {
    try {
      executor.execute(() -> drain(first));
    } catch (RejectedExecutionException rejected) {
      recordEncodingFailure(first.sourceGeneration());
      Work next = workSlot.completeAndTakeNext();
      while (next != null) {
        recordEncodingFailure(next.sourceGeneration());
        next = workSlot.completeAndTakeNext();
      }
    }
  }

  private void drain(Work first) {
    Work current = first;
    while (current != null) {
      encode(current);
      current = workSlot.completeAndTakeNext();
    }
  }

  private void encode(Work work) {
    synchronized (stateLock) {
      encodingAttempts = increment(encodingAttempts);
    }
    try {
      byte[] jpeg =
          Objects.requireNonNull(
              encoder.encodeNv21(
                  work.nv21(),
                  work.width(),
                  work.height(),
                  config.jpegQuality(),
                  config.maximumJpegBytes()),
              "setup preview encoder result");
      if (!validJpeg(jpeg) || jpeg.length > config.maximumJpegBytes()) {
        throw new IllegalArgumentException("setup preview encoder returned an invalid JPEG");
      }
      synchronized (stateLock) {
        if (!closed
            && sourceReason == Reason.NONE
            && work.sourceGeneration() == sourceGeneration) {
          publishLocked(work.sourceGeneration(), work.timestampBoottimeNanos(), jpeg);
        }
      }
    } catch (RuntimeException failure) {
      recordEncodingFailure(work.sourceGeneration());
    }
  }

  private void recordEncodingFailure(long failedGeneration) {
    synchronized (stateLock) {
      encodingFailures = increment(encodingFailures);
      if (!closed
          && sourceReason == Reason.NONE
          && failedGeneration == sourceGeneration
          && publishedJpeg.length == 0) {
        noFrameReason = Reason.ENCODING_FAILED;
      }
    }
  }

  private void publishLocked(long generation, long timestamp, byte[] jpeg) {
    if (generation != sourceGeneration || timestamp <= publishedTimestampNanos) {
      return;
    }
    publishedTimestampNanos = timestamp;
    publishedJpeg = jpeg.clone();
    publishedFrames = increment(publishedFrames);
    noFrameReason = Reason.NO_FRAME;
  }

  private boolean cadenceAllowsLocked(long timestamp) {
    if (lastAcceptedTimestampNanos < 0) {
      return true;
    }
    if (timestamp <= lastAcceptedTimestampNanos) {
      throw new IllegalArgumentException("setup preview source timestamps must increase");
    }
    return timestamp - lastAcceptedTimestampNanos >= config.minimumFrameIntervalNanos();
  }

  private Snapshot unavailableSnapshot(Reason reason, Metrics metrics) {
    return new Snapshot(
        State.UNAVAILABLE, reason, sourceGeneration, -1, -1, new byte[0], metrics);
  }

  private Metrics metricsLocked() {
    return new Metrics(
        offeredFrames,
        cadenceSkippedFrames,
        replacedPendingFrames,
        encodingAttempts,
        encodingFailures,
        publishedFrames,
        workSlot.retainedValueCount());
  }

  private static int exactNv21Bytes(int width, int height) {
    if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0) {
      throw new IllegalArgumentException("setup preview NV21 dimensions must be positive and even");
    }
    try {
      long pixels = Math.multiplyExact((long) width, height);
      long bytes = Math.multiplyExact(pixels, 3L) / 2L;
      if (bytes > Integer.MAX_VALUE) {
        throw new IllegalArgumentException("setup preview NV21 input is too large");
      }
      return (int) bytes;
    } catch (ArithmeticException overflow) {
      throw new IllegalArgumentException("setup preview NV21 dimensions overflow", overflow);
    }
  }

  private static boolean validJpeg(byte[] jpeg) {
    return jpeg.length >= 4
        && (jpeg[0] & 0xff) == 0xff
        && (jpeg[1] & 0xff) == 0xd8
        && (jpeg[jpeg.length - 2] & 0xff) == 0xff
        && (jpeg[jpeg.length - 1] & 0xff) == 0xd9;
  }

  private static long increment(long value) {
    return value == Long.MAX_VALUE ? Long.MAX_VALUE : value + 1;
  }

  private static final class PreviewThreadFactory implements ThreadFactory {
    @Override
    public Thread newThread(Runnable runnable) {
      Thread thread = new Thread(runnable, "setup-preview-encoder");
      thread.setDaemon(true);
      return thread;
    }
  }
}
