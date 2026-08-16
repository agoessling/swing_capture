package com.agoessling.swingcapture;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;

/** Fixed-memory ordinal correlator for a running Camera2-to-MediaCodec stream. */
public final class StreamingTimestampCalibrator {
  private static final int EVIDENCE_CAPACITY = 64;
  private static final int CORRELATION_CAPACITY = 256;
  public static final int DIAGNOSTIC_SAMPLE_CAPACITY = 6;
  private static final long MAXIMUM_OFFSET_SPAN_NANOS = 1_000_000L;
  private static final int MAXIMUM_CONSECUTIVE_ESTABLISHED_OUTLIERS = 2;

  private final long[] encoderOrdinals = new long[CORRELATION_CAPACITY];
  private final long[] encoderPtsUs = new long[CORRELATION_CAPACITY];
  private final long[] cameraOrdinals = new long[CORRELATION_CAPACITY];
  private final long[] cameraSensorNs = new long[CORRELATION_CAPACITY];
  private final long[] offsets = new long[EVIDENCE_CAPACITY];
  private int offsetHead;
  private int offsetCount;
  private int consecutiveEstablishedOutliers;
  private long lastEncoderOrdinal = -1;
  private long lastEncoderPtsUs = -1;
  private long lastCameraOrdinal = -1;
  private long lastCameraSensorNs = -1;

  /** One exact MediaCodec output timestamp retained in the bounded correlation ring. */
  public record EncoderSample(long ordinal, long presentationTimeUs) {
    public EncoderSample {
      if (ordinal < 0 || presentationTimeUs < 0) {
        throw new IllegalArgumentException("encoder sample must be nonnegative");
      }
    }
  }

  /** One exact Camera2 completion timestamp retained in the bounded correlation ring. */
  public record CameraSample(long frameNumber, long sensorTimestampNs) {
    public CameraSample {
      if (frameNumber < 0 || sensorTimestampNs < 0) {
        throw new IllegalArgumentException("camera sample must be nonnegative");
      }
    }
  }

  /**
   * Exact bounded evidence nearest one encoder access unit, plus the latest observation in each
   * asynchronous stream. Nearest samples are sorted by ordinal/frame number in the returned lists.
   */
  public record DiagnosticSnapshot(
      long requestedEncoderOrdinal,
      List<EncoderSample> nearestEncoderSamples,
      List<CameraSample> nearestCameraSamples,
      EncoderSample latestEncoderSample,
      CameraSample latestCameraSample) {
    public DiagnosticSnapshot {
      if (requestedEncoderOrdinal < 0) {
        throw new IllegalArgumentException("requested encoder ordinal must be nonnegative");
      }
      nearestEncoderSamples = List.copyOf(nearestEncoderSamples);
      nearestCameraSamples = List.copyOf(nearestCameraSamples);
      if (nearestEncoderSamples.size() > DIAGNOSTIC_SAMPLE_CAPACITY
          || nearestCameraSamples.size() > DIAGNOSTIC_SAMPLE_CAPACITY) {
        throw new IllegalArgumentException("diagnostic sample list exceeds its fixed bound");
      }
    }
  }

  public StreamingTimestampCalibrator() {
    Arrays.fill(encoderOrdinals, -1);
    Arrays.fill(cameraOrdinals, -1);
  }

  public synchronized void observeEncoder(long ordinal, long presentationTimeUs) {
    if (ordinal < 0 || presentationTimeUs < 0) {
      throw new IllegalArgumentException("encoder timestamp metadata must be nonnegative");
    }
    if (ordinal != lastEncoderOrdinal + 1 || presentationTimeUs <= lastEncoderPtsUs) {
      throw new IllegalStateException("encoder ordinals and timestamps must be continuous");
    }
    lastEncoderOrdinal = ordinal;
    lastEncoderPtsUs = presentationTimeUs;
    int slot = slot(ordinal);
    encoderOrdinals[slot] = ordinal;
    encoderPtsUs[slot] = presentationTimeUs;
    correlate(ordinal);
  }

  public synchronized void observeCamera(long frameNumber, long sensorTimestampNanos) {
    if (frameNumber < 0 || sensorTimestampNanos < 0) {
      throw new IllegalArgumentException("camera timestamp metadata must be nonnegative");
    }
    if (frameNumber <= lastCameraOrdinal || sensorTimestampNanos <= lastCameraSensorNs) {
      throw new IllegalStateException("camera frame numbers and timestamps must increase");
    }
    lastCameraOrdinal = frameNumber;
    lastCameraSensorNs = sensorTimestampNanos;
    int slot = slot(frameNumber);
    cameraOrdinals[slot] = frameNumber;
    cameraSensorNs[slot] = sensorTimestampNanos;
    correlate(frameNumber);
  }

  public synchronized boolean valid() {
    return offsetCount >= 2 && offsetSpanNanos() <= MAXIMUM_OFFSET_SPAN_NANOS;
  }

  public synchronized long sensorTimestampNanos(long presentationTimeUs) {
    if (!valid()) {
      throw new IllegalStateException("Camera2/MediaCodec clock mapping is not validated");
    }
    return Math.addExact(Math.multiplyExact(presentationTimeUs, 1_000L), medianOffsetNanos());
  }

  public synchronized long presentationTimeUs(long sensorTimestampNanos) {
    if (!valid()) {
      throw new IllegalStateException("Camera2/MediaCodec clock mapping is not validated");
    }
    return Math.floorDiv(sensorTimestampNanos - medianOffsetNanos(), 1_000L);
  }

  public synchronized long medianOffsetNanos() {
    if (offsetCount == 0) {
      throw new IllegalStateException("no timestamp evidence");
    }
    long[] sorted = evidenceCopy();
    Arrays.sort(sorted);
    return sorted[sorted.length / 2];
  }

  public synchronized long offsetSpanNanos() {
    if (offsetCount == 0) {
      return Long.MAX_VALUE;
    }
    long minimum = Long.MAX_VALUE;
    long maximum = Long.MIN_VALUE;
    for (long value : evidenceCopy()) {
      minimum = Math.min(minimum, value);
      maximum = Math.max(maximum, value);
    }
    return maximum - minimum;
  }

  public synchronized int evidenceCount() {
    return offsetCount;
  }

  /** Takes a fixed-size exact snapshot without changing correlation or mapping state. */
  public synchronized DiagnosticSnapshot diagnosticSnapshot(long requestedEncoderOrdinal) {
    if (requestedEncoderOrdinal < 0) {
      throw new IllegalArgumentException("requested encoder ordinal must be nonnegative");
    }
    List<EncoderSample> encoderSamples = new ArrayList<>(CORRELATION_CAPACITY);
    List<CameraSample> cameraSamples = new ArrayList<>(CORRELATION_CAPACITY);
    for (int index = 0; index < CORRELATION_CAPACITY; ++index) {
      if (encoderOrdinals[index] >= 0) {
        encoderSamples.add(new EncoderSample(encoderOrdinals[index], encoderPtsUs[index]));
      }
      if (cameraOrdinals[index] >= 0) {
        cameraSamples.add(new CameraSample(cameraOrdinals[index], cameraSensorNs[index]));
      }
    }
    return new DiagnosticSnapshot(
        requestedEncoderOrdinal,
        nearestEncoderSamples(encoderSamples, requestedEncoderOrdinal),
        nearestCameraSamples(cameraSamples, requestedEncoderOrdinal),
        lastEncoderOrdinal < 0
            ? null
            : new EncoderSample(lastEncoderOrdinal, lastEncoderPtsUs),
        lastCameraOrdinal < 0
            ? null
            : new CameraSample(lastCameraOrdinal, lastCameraSensorNs));
  }

  private void correlate(long ordinal) {
    int slot = slot(ordinal);
    if (encoderOrdinals[slot] != ordinal || cameraOrdinals[slot] != ordinal) {
      return;
    }
    long offset = cameraSensorNs[slot] - Math.multiplyExact(encoderPtsUs[slot], 1_000L);
    long candidateSpanNanos = spanWith(offset);
    if (offsetCount == 1 && candidateSpanNanos > MAXIMUM_OFFSET_SPAN_NANOS) {
      // Before the mapping is established, prefer a new candidate cluster over retaining a
      // solitary asynchronous startup mismatch for the full evidence window.
      offsetHead = 0;
      offsets[0] = offset;
      consecutiveEstablishedOutliers = 0;
      return;
    }
    if (offsetCount >= 2 && candidateSpanNanos > MAXIMUM_OFFSET_SPAN_NANOS) {
      // Once a coherent mapping exists, an isolated ordinal-correlation mismatch must not poison
      // the rolling window and suspend retention. Sustained mismatches are not safe to ignore:
      // they can indicate a permanent Camera2/encoder ordinal shift or a changed clock mapping.
      ++consecutiveEstablishedOutliers;
      if (consecutiveEstablishedOutliers > MAXIMUM_CONSECUTIVE_ESTABLISHED_OUTLIERS) {
        throw new IllegalStateException(
            "Camera2/MediaCodec timestamp correlation produced "
                + consecutiveEstablishedOutliers
                + " consecutive outliers at encoder ordinal "
                + ordinal
                + " (candidate_offset_ns="
                + offset
                + ", candidate_span_ns="
                + candidateSpanNanos
                + ")");
      }
      return;
    }
    consecutiveEstablishedOutliers = 0;
    if (offsetCount == offsets.length) {
      offsets[offsetHead] = offset;
      offsetHead = (offsetHead + 1) % offsets.length;
    } else {
      offsets[(offsetHead + offsetCount) % offsets.length] = offset;
      ++offsetCount;
    }
  }

  private long spanWith(long candidate) {
    long minimum = candidate;
    long maximum = candidate;
    for (long value : evidenceCopy()) {
      minimum = Math.min(minimum, value);
      maximum = Math.max(maximum, value);
    }
    return maximum - minimum;
  }

  private long[] evidenceCopy() {
    long[] copy = new long[offsetCount];
    for (int index = 0; index < offsetCount; ++index) {
      copy[index] = offsets[(offsetHead + index) % offsets.length];
    }
    return copy;
  }

  private static List<EncoderSample> nearestEncoderSamples(
      List<EncoderSample> samples, long requestedOrdinal) {
    samples.sort(
        Comparator.comparingLong(
                (EncoderSample sample) -> distance(sample.ordinal(), requestedOrdinal))
            .thenComparingLong(EncoderSample::ordinal));
    List<EncoderSample> nearest =
        new ArrayList<>(
            samples.subList(0, Math.min(samples.size(), DIAGNOSTIC_SAMPLE_CAPACITY)));
    nearest.sort(Comparator.comparingLong(EncoderSample::ordinal));
    return nearest;
  }

  private static List<CameraSample> nearestCameraSamples(
      List<CameraSample> samples, long requestedOrdinal) {
    samples.sort(
        Comparator.comparingLong(
                (CameraSample sample) -> distance(sample.frameNumber(), requestedOrdinal))
            .thenComparingLong(CameraSample::frameNumber));
    List<CameraSample> nearest =
        new ArrayList<>(
            samples.subList(0, Math.min(samples.size(), DIAGNOSTIC_SAMPLE_CAPACITY)));
    nearest.sort(Comparator.comparingLong(CameraSample::frameNumber));
    return nearest;
  }

  private static long distance(long first, long second) {
    return first >= second ? first - second : second - first;
  }

  private static int slot(long ordinal) {
    return Math.floorMod(ordinal, CORRELATION_CAPACITY);
  }
}
