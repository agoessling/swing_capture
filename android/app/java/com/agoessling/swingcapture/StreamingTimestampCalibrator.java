package com.agoessling.swingcapture;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/** Fixed-memory ordinal correlator for a running Camera2-to-MediaCodec stream. */
public final class StreamingTimestampCalibrator {
  private static final int EVIDENCE_CAPACITY = 64;
  private static final int CORRELATION_CAPACITY = 256;
  public static final int DIAGNOSTIC_SAMPLE_CAPACITY = 6;
  private static final long MAXIMUM_OFFSET_SPAN_NANOS = 1_000_000L;
  private static final long MAXIMUM_STARTUP_ALIGNMENT_DISTANCE_NANOS = 1_000_000L;
  private static final int MINIMUM_STARTUP_ALIGNMENT_PAIRS = 2;
  private static final int MAXIMUM_CONSECUTIVE_ESTABLISHED_OUTLIERS = 2;

  private final long[] encoderOrdinals = new long[CORRELATION_CAPACITY];
  private final long[] encoderPtsUs = new long[CORRELATION_CAPACITY];
  private final long[] cameraOrdinals = new long[CORRELATION_CAPACITY];
  private final long[] cameraSensorNs = new long[CORRELATION_CAPACITY];
  private final long[] offsets = new long[EVIDENCE_CAPACITY];
  private int offsetHead;
  private int offsetCount;
  private int consecutiveEstablishedOutliers;
  private final boolean alignStartupByTimestamp;
  private final long expectedStartupOffsetNanos;
  private long cameraToEncoderOrdinalShift = Long.MIN_VALUE;
  private long lastCorrelatedEncoderOrdinal = -1;
  private long startupBestDistanceNanos = Long.MAX_VALUE;
  private long startupBestEncoderOrdinal = -1;
  private long startupBestCameraOrdinal = -1;
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
    this(false, 0);
  }

  /**
   * Creates a correlator, optionally discovering a fixed warm-session startup frame shift.
   *
   * <p>A constrained high-speed session can begin delivering Camera2 callbacks several frames
   * before its encoder surface emits an access unit. When both timestamps use Android's realtime
   * clock, timestamp startup alignment pairs the nearest initial Camera2 timestamp with the first
   * encoder PTS and then applies that fixed ordinal shift to the stream. It must only be enabled
   * after verifying {@code SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME}.
   */
  public StreamingTimestampCalibrator(boolean alignStartupByTimestamp) {
    this(alignStartupByTimestamp, 0);
  }

  /** Creates a warm-stream correlator around a measured camera-clock minus encoder-clock offset. */
  public StreamingTimestampCalibrator(long expectedStartupOffsetNanos) {
    this(true, expectedStartupOffsetNanos);
  }

  private StreamingTimestampCalibrator(
      boolean alignStartupByTimestamp, long expectedStartupOffsetNanos) {
    this.alignStartupByTimestamp = alignStartupByTimestamp;
    this.expectedStartupOffsetNanos = expectedStartupOffsetNanos;
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
    correlateAfterObservation(ordinal);
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
    correlateAfterObservation(frameNumber);
  }

  public synchronized boolean valid() {
    return offsetCount >= 2
        && offsetSpanNanos() <= MAXIMUM_OFFSET_SPAN_NANOS
        && (!alignStartupByTimestamp
            || distance(medianOffsetNanos(), expectedStartupOffsetNanos)
                <= MAXIMUM_STARTUP_ALIGNMENT_DISTANCE_NANOS);
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

  /** Camera ordinal minus encoder ordinal for the established mapping. */
  public synchronized long cameraToEncoderOrdinalShift() {
    if (!alignStartupByTimestamp) {
      return 0;
    }
    if (cameraToEncoderOrdinalShift == Long.MIN_VALUE) {
      throw new IllegalStateException("warm timestamp startup alignment is not established");
    }
    return cameraToEncoderOrdinalShift;
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

  private void correlateAfterObservation(long observedOrdinal) {
    if (!alignStartupByTimestamp) {
      correlateSameOrdinal(observedOrdinal);
      return;
    }
    if (cameraToEncoderOrdinalShift == Long.MIN_VALUE) {
      establishWarmOrdinalShift();
    }
    if (cameraToEncoderOrdinalShift != Long.MIN_VALUE) {
      correlateAlignedOrdinals();
    } else if (lastEncoderOrdinal >= EVIDENCE_CAPACITY
        && lastCameraOrdinal >= EVIDENCE_CAPACITY) {
      throw new IllegalStateException(
          "Camera2/MediaCodec warm startup timestamps could not be aligned within "
              + MAXIMUM_STARTUP_ALIGNMENT_DISTANCE_NANOS
              + " ns (nearest_distance_ns="
              + startupBestDistanceNanos
              + ", nearest_encoder_ordinal="
              + startupBestEncoderOrdinal
              + ", nearest_camera_ordinal="
              + startupBestCameraOrdinal
              + ", timestamp_samples="
              + diagnosticSnapshot(0)
              + ")");
    }
  }

  private void establishWarmOrdinalShift() {
    long nearestDistance = Long.MAX_VALUE;
    long nearestEncoderOrdinal = -1;
    long nearestCameraOrdinal = -1;
    long selectedShift = Long.MIN_VALUE;
    int selectedSupport = 0;
    long selectedMaximumDistance = Long.MAX_VALUE;
    long selectedSpan = Long.MAX_VALUE;
    boolean selectedTied = false;
    Set<Long> evaluatedShifts = new HashSet<>();
    for (int encoderIndex = 0; encoderIndex < CORRELATION_CAPACITY; ++encoderIndex) {
      if (encoderOrdinals[encoderIndex] < 0) {
        continue;
      }
      long encoderTimestampNanos = Math.multiplyExact(encoderPtsUs[encoderIndex], 1_000L);
      for (int cameraIndex = 0; cameraIndex < CORRELATION_CAPACITY; ++cameraIndex) {
        if (cameraOrdinals[cameraIndex] < 0) {
          continue;
        }
        long candidateOffset = cameraSensorNs[cameraIndex] - encoderTimestampNanos;
        long candidateDistance = distance(candidateOffset, expectedStartupOffsetNanos);
        if (candidateDistance < nearestDistance) {
          nearestDistance = candidateDistance;
          nearestEncoderOrdinal = encoderOrdinals[encoderIndex];
          nearestCameraOrdinal = cameraOrdinals[cameraIndex];
        }
        if (candidateDistance > MAXIMUM_STARTUP_ALIGNMENT_DISTANCE_NANOS) {
          continue;
        }
        long candidateShift =
            Math.subtractExact(cameraOrdinals[cameraIndex], encoderOrdinals[encoderIndex]);
        if (!evaluatedShifts.add(candidateShift)) {
          continue;
        }
        int support = 0;
        long minimumOffset = Long.MAX_VALUE;
        long maximumOffset = Long.MIN_VALUE;
        long maximumDistance = 0;
        for (int supportEncoderIndex = 0;
            supportEncoderIndex < CORRELATION_CAPACITY;
            ++supportEncoderIndex) {
          long supportEncoderOrdinal = encoderOrdinals[supportEncoderIndex];
          if (supportEncoderOrdinal < 0) {
            continue;
          }
          long supportCameraOrdinal = Math.addExact(supportEncoderOrdinal, candidateShift);
          if (supportCameraOrdinal < 0) {
            continue;
          }
          int supportCameraIndex = slot(supportCameraOrdinal);
          if (cameraOrdinals[supportCameraIndex] != supportCameraOrdinal) {
            continue;
          }
          long supportOffset =
              cameraSensorNs[supportCameraIndex]
                  - Math.multiplyExact(encoderPtsUs[supportEncoderIndex], 1_000L);
          long supportDistance = distance(supportOffset, expectedStartupOffsetNanos);
          if (supportDistance > MAXIMUM_STARTUP_ALIGNMENT_DISTANCE_NANOS) {
            continue;
          }
          ++support;
          minimumOffset = Math.min(minimumOffset, supportOffset);
          maximumOffset = Math.max(maximumOffset, supportOffset);
          maximumDistance = Math.max(maximumDistance, supportDistance);
        }
        long span = support == 0 ? Long.MAX_VALUE : maximumOffset - minimumOffset;
        if (support < MINIMUM_STARTUP_ALIGNMENT_PAIRS || span > MAXIMUM_OFFSET_SPAN_NANOS) {
          continue;
        }
        if (support > selectedSupport
            || (support == selectedSupport && maximumDistance < selectedMaximumDistance)
            || (support == selectedSupport
                && maximumDistance == selectedMaximumDistance
                && span < selectedSpan)) {
          selectedShift = candidateShift;
          selectedSupport = support;
          selectedMaximumDistance = maximumDistance;
          selectedSpan = span;
          selectedTied = false;
        } else if (support == selectedSupport
            && maximumDistance == selectedMaximumDistance
            && span == selectedSpan
            && candidateShift != selectedShift) {
          selectedTied = true;
        }
      }
    }
    if (!selectedTied && selectedSupport >= MINIMUM_STARTUP_ALIGNMENT_PAIRS) {
      cameraToEncoderOrdinalShift = selectedShift;
      // A negative shift means the encoder emitted startup access units before the first Camera2
      // callback exposed to this session. Begin at the first encoder ordinal that has a
      // nonnegative matching Camera2 ordinal instead of waiting forever for impossible samples.
      lastCorrelatedEncoderOrdinal = Math.max(0L, -cameraToEncoderOrdinalShift) - 1L;
    }
    startupBestDistanceNanos = nearestDistance;
    startupBestEncoderOrdinal = nearestEncoderOrdinal;
    startupBestCameraOrdinal = nearestCameraOrdinal;
  }

  private void correlateAlignedOrdinals() {
    // Constrained high-speed Camera2 may expose one metadata callback per eight-frame burst while
    // MediaCodec emits every frame. Correlate every available shifted pair in order, but do not
    // require camera metadata for the intervening encoder ordinals.
    for (long encoderOrdinal = lastCorrelatedEncoderOrdinal + 1;
        encoderOrdinal <= lastEncoderOrdinal;
        ++encoderOrdinal) {
      long cameraOrdinal = Math.addExact(encoderOrdinal, cameraToEncoderOrdinalShift);
      int encoderSlot = slot(encoderOrdinal);
      int cameraSlot = slot(cameraOrdinal);
      if (encoderOrdinals[encoderSlot] != encoderOrdinal
          || cameraOrdinals[cameraSlot] != cameraOrdinal) {
        continue;
      }
      addOffset(
          encoderOrdinal,
          cameraSensorNs[cameraSlot]
              - Math.multiplyExact(encoderPtsUs[encoderSlot], 1_000L));
      lastCorrelatedEncoderOrdinal = encoderOrdinal;
    }
  }

  private void correlateSameOrdinal(long ordinal) {
    if (ordinal < 0) {
      return;
    }
    int slot = slot(ordinal);
    if (encoderOrdinals[slot] != ordinal || cameraOrdinals[slot] != ordinal) {
      return;
    }
    long offset = cameraSensorNs[slot] - Math.multiplyExact(encoderPtsUs[slot], 1_000L);
    addOffset(ordinal, offset);
  }

  private void addOffset(long ordinal, long offset) {
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
