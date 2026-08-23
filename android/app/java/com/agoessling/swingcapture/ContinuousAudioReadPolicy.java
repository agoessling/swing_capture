package com.agoessling.swingcapture;

/** Bounds blocking high-speed audio reads independently of AudioRecord's safety-buffer size. */
final class ContinuousAudioReadPolicy {
  static final int TARGET_READ_DURATION_MILLIS = 20;

  private ContinuousAudioReadPolicy() {}

  static int framesPerRead(int sampleRateHz) {
    if (sampleRateHz <= 0) {
      throw new IllegalArgumentException("sampleRateHz must be positive");
    }
    long numerator = (long) sampleRateHz * TARGET_READ_DURATION_MILLIS;
    long frames = Math.max(1L, (numerator + 999L) / 1_000L);
    if (frames > Integer.MAX_VALUE) {
      throw new IllegalArgumentException("audio read frame count exceeds int range");
    }
    return (int) frames;
  }
}
