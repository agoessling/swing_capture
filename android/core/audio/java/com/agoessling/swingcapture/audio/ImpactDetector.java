package com.agoessling.swingcapture.audio;

import java.util.Objects;

/** Stateful impact detector for one continuous 48 kHz, mono, signed PCM16 stream. */
public final class ImpactDetector {
  public static final int SAMPLE_RATE_HZ = 48_000;

  /** Detector tuning expressed in normalized PCM amplitude and sample-frame counts. */
  public record Config(
      float thresholdMultiplier,
      float minimumPeakAmplitude,
      float initialNoiseFloor,
      float noiseUpdateClipMultiplier,
      double noiseFloorTimeConstantSeconds,
      int peakConfirmationFrames,
      long cooldownFrames) {
    public Config {
      requireFinitePositive(thresholdMultiplier, "thresholdMultiplier");
      requireNormalized(minimumPeakAmplitude, "minimumPeakAmplitude");
      requireNormalized(initialNoiseFloor, "initialNoiseFloor");
      if (!Float.isFinite(noiseUpdateClipMultiplier) || noiseUpdateClipMultiplier < 1.0f) {
        throw new IllegalArgumentException("noiseUpdateClipMultiplier must be at least one");
      }
      if (!Double.isFinite(noiseFloorTimeConstantSeconds)
          || noiseFloorTimeConstantSeconds <= 0.0) {
        throw new IllegalArgumentException("noiseFloorTimeConstantSeconds must be positive");
      }
      if (peakConfirmationFrames < 0) {
        throw new IllegalArgumentException("peakConfirmationFrames cannot be negative");
      }
      if (cooldownFrames < 0) {
        throw new IllegalArgumentException("cooldownFrames cannot be negative");
      }
    }

    public static Config defaults() {
      // Pixel 6 UNPROCESSED AudioRecord HIL measures the fixture impact near 0.024 normalized
      // amplitude and quiet-room noise near 0.00053. Keep useful absolute margin while the
      // adaptive multiplier remains the primary gate in noisier environments.
      return new Config(8.0f, 0.015f, 0.005f, 4.0f, 0.5, 72, 12_000);
    }

    private static void requireFinitePositive(float value, String name) {
      if (!Float.isFinite(value) || value <= 0.0f) {
        throw new IllegalArgumentException(name + " must be positive");
      }
    }

    private static void requireNormalized(float value, String name) {
      if (!Float.isFinite(value) || value < 0.0f || value > 1.0f) {
        throw new IllegalArgumentException(name + " must be between zero and one");
      }
    }
  }

  /** A confirmed peak, retaining both its estimated strike sample and delivery sample. */
  public record Impact(
      long strikeFramePosition,
      long confirmationFramePosition,
      long sourceBlockStartFramePosition,
      int sampleIndexInBlock,
      float peakAmplitude,
      float noiseFloorAtDetection,
      float thresholdAtDetection) {
    public Impact {
      if (confirmationFramePosition < strikeFramePosition) {
        throw new IllegalArgumentException("confirmation cannot precede strike");
      }
      if (sampleIndexInBlock < 0) {
        throw new IllegalArgumentException("sampleIndexInBlock cannot be negative");
      }
    }
  }

  /** Callback boundary that avoids allocating a per-block event collection. */
  @FunctionalInterface
  public interface ImpactSink {
    void onImpact(Impact impact);
  }

  public record ProcessResult(
      int eventsDetected,
      float blockPeakAmplitude,
      float noiseFloorAfterBlock,
      float thresholdAfterBlock) {
    public ProcessResult {
      if (eventsDetected < 0) {
        throw new IllegalArgumentException("eventsDetected cannot be negative");
      }
      if (!Float.isFinite(blockPeakAmplitude)
          || blockPeakAmplitude < 0.0f
          || blockPeakAmplitude > 1.0f) {
        throw new IllegalArgumentException("blockPeakAmplitude must be between zero and one");
      }
      if (!Float.isFinite(noiseFloorAfterBlock)
          || noiseFloorAfterBlock < 0.0f
          || noiseFloorAfterBlock > 1.0f) {
        throw new IllegalArgumentException("noiseFloorAfterBlock must be between zero and one");
      }
      if (!Float.isFinite(thresholdAfterBlock)
          || thresholdAfterBlock < 0.0f
          || thresholdAfterBlock > 1.0f) {
        throw new IllegalArgumentException("thresholdAfterBlock must be between zero and one");
      }
    }
  }

  private final Config config;
  private final float noiseSmoothingFactor;
  private float noiseFloor;
  private boolean hasProcessedSample;
  private long lastFramePosition;
  private long cooldownUntilFramePosition;
  private boolean candidateActive;
  private long candidateStrikeFramePosition;
  private long candidateSourceBlockStartFramePosition;
  private int candidateSampleIndexInBlock;
  private float candidatePeakAmplitude;
  private float candidateNoiseFloor;
  private float candidateThreshold;
  private long candidateConfirmationDeadline;

  public ImpactDetector() {
    this(Config.defaults());
  }

  public ImpactDetector(Config config) {
    this.config = Objects.requireNonNull(config, "config");
    noiseSmoothingFactor =
        (float)
            -Math.expm1(
                -1.0 / (config.noiseFloorTimeConstantSeconds() * SAMPLE_RATE_HZ));
    reset();
  }

  /**
   * Processes a block read from AudioRecord.
   *
   * <p>{@code firstFramePosition} is the absolute AudioRecord frame position corresponding to
   * {@code pcm[offset]}. Non-empty blocks must not overlap or move backward. Gaps are permitted and
   * explicitly advance pending confirmation and cooldown state.
   */
  public ProcessResult processBlock(
      short[] pcm, int offset, int length, long firstFramePosition, ImpactSink sink) {
    Objects.requireNonNull(pcm, "pcm");
    Objects.requireNonNull(sink, "sink");
    Objects.checkFromIndexSize(offset, length, pcm.length);
    if (firstFramePosition < 0) {
      throw new IllegalArgumentException("firstFramePosition cannot be negative");
    }
    if (length > 0 && firstFramePosition > Long.MAX_VALUE - (length - 1L)) {
      throw new IllegalArgumentException("block frame positions overflow long");
    }
    if (length > 0 && hasProcessedSample && firstFramePosition <= lastFramePosition) {
      throw new IllegalArgumentException(
          "audio blocks must have monotonically increasing frame positions");
    }

    int events = 0;
    float blockPeakAmplitude = 0.0f;
    for (int blockIndex = 0; blockIndex < length; ++blockIndex) {
      long framePosition = firstFramePosition + blockIndex;
      float amplitude = normalizedAmplitude(pcm[offset + blockIndex]);
      blockPeakAmplitude = Math.max(blockPeakAmplitude, amplitude);

      // A block gap can jump past confirmation. Emit the prior peak before this later sample is
      // allowed to start a distinct candidate.
      if (candidateActive && framePosition >= candidateConfirmationDeadline) {
        emitCandidate(framePosition, sink);
        ++events;
      }

      if (candidateActive) {
        if (amplitude > candidatePeakAmplitude) {
          updateCandidate(
              amplitude, framePosition, firstFramePosition, blockIndex);
        }
        if (framePosition >= candidateConfirmationDeadline) {
          emitCandidate(framePosition, sink);
          ++events;
        }
      } else {
        float threshold = detectionThreshold();
        if (framePosition >= cooldownUntilFramePosition && amplitude >= threshold) {
          beginCandidate(
              amplitude, threshold, framePosition, firstFramePosition, blockIndex);
          if (framePosition >= candidateConfirmationDeadline) {
            emitCandidate(framePosition, sink);
            ++events;
          }
        } else {
          updateNoiseFloor(amplitude);
        }
      }

      hasProcessedSample = true;
      lastFramePosition = framePosition;
    }
    return new ProcessResult(events, blockPeakAmplitude, noiseFloor, detectionThreshold());
  }

  public void reset() {
    noiseFloor = config.initialNoiseFloor();
    hasProcessedSample = false;
    lastFramePosition = 0;
    cooldownUntilFramePosition = 0;
    candidateActive = false;
    candidateStrikeFramePosition = 0;
    candidateSourceBlockStartFramePosition = 0;
    candidateSampleIndexInBlock = 0;
    candidatePeakAmplitude = 0.0f;
    candidateNoiseFloor = 0.0f;
    candidateThreshold = 0.0f;
    candidateConfirmationDeadline = 0;
  }

  public float noiseFloor() {
    return noiseFloor;
  }

  public float detectionThreshold() {
    return Math.max(config.minimumPeakAmplitude(), noiseFloor * config.thresholdMultiplier());
  }

  public boolean confirmationPending() {
    return candidateActive;
  }

  public long cooldownUntilFramePosition() {
    return cooldownUntilFramePosition;
  }

  public Config config() {
    return config;
  }

  private static float normalizedAmplitude(short sample) {
    int widened = sample;
    int magnitude = widened < 0 ? -widened : widened;
    return magnitude / 32768.0f;
  }

  private void updateNoiseFloor(float amplitude) {
    float clippingBasis = Math.max(noiseFloor, config.initialNoiseFloor());
    float clippedAmplitude =
        Math.min(amplitude, clippingBasis * config.noiseUpdateClipMultiplier());
    noiseFloor += noiseSmoothingFactor * (clippedAmplitude - noiseFloor);
    noiseFloor = Math.max(0.0f, Math.min(1.0f, noiseFloor));
  }

  private void beginCandidate(
      float amplitude,
      float threshold,
      long framePosition,
      long blockStartFramePosition,
      int sampleIndexInBlock) {
    candidateActive = true;
    candidateStrikeFramePosition = framePosition;
    candidateSourceBlockStartFramePosition = blockStartFramePosition;
    candidateSampleIndexInBlock = sampleIndexInBlock;
    candidatePeakAmplitude = amplitude;
    candidateNoiseFloor = noiseFloor;
    candidateThreshold = threshold;
    candidateConfirmationDeadline = saturatingAdd(framePosition, config.peakConfirmationFrames());
  }

  private void updateCandidate(
      float amplitude,
      long framePosition,
      long blockStartFramePosition,
      int sampleIndexInBlock) {
    candidateStrikeFramePosition = framePosition;
    candidateSourceBlockStartFramePosition = blockStartFramePosition;
    candidateSampleIndexInBlock = sampleIndexInBlock;
    candidatePeakAmplitude = amplitude;
    candidateConfirmationDeadline = saturatingAdd(framePosition, config.peakConfirmationFrames());
  }

  private void emitCandidate(long confirmationFramePosition, ImpactSink sink) {
    Impact impact =
        new Impact(
            candidateStrikeFramePosition,
            confirmationFramePosition,
            candidateSourceBlockStartFramePosition,
            candidateSampleIndexInBlock,
            candidatePeakAmplitude,
            candidateNoiseFloor,
            candidateThreshold);
    cooldownUntilFramePosition =
        saturatingAdd(candidateStrikeFramePosition, config.cooldownFrames());
    candidateActive = false;
    sink.onImpact(impact);
  }

  private static long saturatingAdd(long value, long increment) {
    if (increment > Long.MAX_VALUE - value) {
      return Long.MAX_VALUE;
    }
    return value + increment;
  }
}
