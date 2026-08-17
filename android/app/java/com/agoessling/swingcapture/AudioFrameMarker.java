package com.agoessling.swingcapture;

import java.util.Optional;
import java.util.concurrent.TimeUnit;

/** Maps one recent AudioRecord BOOTTIME timestamp to an operator-triggered PCM marker. */
public final class AudioFrameMarker {
  private static final long MAXIMUM_EXTRAPOLATION_NANOS = TimeUnit.MILLISECONDS.toNanos(500);

  public record Marker(long framePosition, long uncertaintyFrames) {
    public Marker {
      if (framePosition < 0 || uncertaintyFrames < 1) {
        throw new IllegalArgumentException("audio marker values must be nonnegative");
      }
    }
  }

  private AudioFrameMarker() {}

  public static Optional<Marker> estimate(
      long requestedBoottimeNanos,
      long timestampFramePosition,
      long timestampBoottimeNanos,
      long timestampUncertaintyNanos,
      int sampleRateHz) {
    if (requestedBoottimeNanos < 0
        || timestampFramePosition < 0
        || timestampBoottimeNanos < 0
        || timestampUncertaintyNanos < 0
        || sampleRateHz <= 0) {
      throw new IllegalArgumentException("audio marker input is invalid");
    }
    long elapsed = requestedBoottimeNanos - timestampBoottimeNanos;
    if (elapsed < 0 || elapsed > MAXIMUM_EXTRAPOLATION_NANOS) {
      return Optional.empty();
    }
    long frameDelta = roundedFrames(elapsed, sampleRateHz);
    long uncertaintyFrames = ceilingFrames(timestampUncertaintyNanos, sampleRateHz) + 1;
    try {
      return Optional.of(
          new Marker(
              Math.addExact(timestampFramePosition, frameDelta), uncertaintyFrames));
    } catch (ArithmeticException overflow) {
      return Optional.empty();
    }
  }

  private static long roundedFrames(long nanos, int sampleRateHz) {
    long scaled = Math.multiplyExact(nanos, sampleRateHz);
    return Math.addExact(scaled, TimeUnit.SECONDS.toNanos(1) / 2)
        / TimeUnit.SECONDS.toNanos(1);
  }

  private static long ceilingFrames(long nanos, int sampleRateHz) {
    long scaled = Math.multiplyExact(nanos, sampleRateHz);
    long second = TimeUnit.SECONDS.toNanos(1);
    return Math.addExact(scaled, second - 1) / second;
  }
}
