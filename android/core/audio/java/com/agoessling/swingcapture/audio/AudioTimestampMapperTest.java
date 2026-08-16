package com.agoessling.swingcapture.audio;

/** Deterministic coverage for bounded BOOTTIME evidence and audio-clock validation. */
public final class AudioTimestampMapperTest {
  private AudioTimestampMapperTest() {}

  public static void main(String[] arguments) {
    requiresTwoConsistentTimestamps();
    mapsFramesWithBoundedUncertainty();
    acceptsSmallClockDrift();
    rejectsBadClockRateWithoutChangingModel();
    rejectsInvalidAndNonmonotonicEvidence();
    boundsRetainedEvidenceAndExtrapolation();
  }

  private static AudioTimestampMapper.Config config(int evidenceCount) {
    return new AudioTimestampMapper.Config(evidenceCount, 2_000_000L, 1_000.0, 500_000_000L);
  }

  private static void requiresTwoConsistentTimestamps() {
    AudioTimestampMapper mapper = new AudioTimestampMapper(config(8));
    check(mapper.observe(0, 10_000_000_000L, 1_000L).accepted(), "first evidence accepted");
    check(!mapper.clockValidated(), "one point cannot validate rate");
    check(mapper.estimateBoottime(0).isEmpty(), "one point cannot map");

    AudioTimestampMapper.ObservationResult second =
        mapper.observe(48_000, 11_000_000_000L, 1_000L);
    check(second.accepted(), "second evidence accepted");
    check(mapper.clockValidated(), "two points validate rate");
    checkNear(mapper.validatedRateHz(), 48_000.0, 0.000001, "validated nominal rate");
  }

  private static void mapsFramesWithBoundedUncertainty() {
    AudioTimestampMapper mapper = new AudioTimestampMapper(config(8));
    mapper.observe(0, 10_000_000_000L, 1_000L);
    mapper.observe(48_000, 11_000_000_000L, 1_000L);

    AudioTimestampMapper.Estimate estimate =
        mapper.estimateBoottime(24_000).orElseThrow();
    check(estimate.boottimeNanos() == 10_500_000_000L, "half-second mapping");
    check(estimate.uncertaintyNanos() == 501_001L, "mapping uncertainty");
    check(estimate.earliestBoottimeNanos() == 10_499_498_999L, "mapping lower bound");
    check(estimate.latestBoottimeNanos() == 10_500_501_001L, "mapping upper bound");
    check(estimate.evidenceCount() == 2, "mapping evidence count");

    AudioTimestampMapper.Estimate anchor = mapper.estimateBoottime(48_000).orElseThrow();
    check(anchor.boottimeNanos() == 11_000_000_000L, "anchor mapping");
    check(anchor.uncertaintyNanos() == 1_001L, "anchor uncertainty");
  }

  private static void acceptsSmallClockDrift() {
    AudioTimestampMapper mapper = new AudioTimestampMapper(config(8));
    mapper.observe(0, 1_000_000_000L, 20_000L);
    AudioTimestampMapper.ObservationResult result =
        mapper.observe(48_000, 1_999_500_000L, 20_000L);

    check(result.accepted(), "500 ppm drift accepted");
    checkNear(result.observedRateHz(), 48_024.012, 0.01, "observed drifted rate");
  }

  private static void rejectsBadClockRateWithoutChangingModel() {
    AudioTimestampMapper mapper = new AudioTimestampMapper(config(8));
    mapper.observe(0, 1_000_000_000L, 10_000L);
    AudioTimestampMapper.ObservationResult rejected =
        mapper.observe(48_000, 2_010_000_000L, 10_000L);

    check(!rejected.accepted(), "bad clock rate rejected");
    check(
        rejected.rejectionReason() == AudioTimestampMapper.RejectionReason.CLOCK_RATE_OUT_OF_RANGE,
        "bad clock reason");
    check(mapper.retainedEvidenceCount() == 1, "rejected evidence not retained");
    check(!mapper.clockValidated(), "rejected evidence does not validate clock");
  }

  private static void rejectsInvalidAndNonmonotonicEvidence() {
    AudioTimestampMapper mapper = new AudioTimestampMapper(config(8));
    checkReason(
        mapper.observe(-1, 1_000_000_000L, 0),
        AudioTimestampMapper.RejectionReason.NEGATIVE_FRAME_POSITION,
        "negative frame");
    checkReason(
        mapper.observe(0, 0, 0),
        AudioTimestampMapper.RejectionReason.INVALID_BOOTTIME,
        "invalid boottime");
    checkReason(
        mapper.observe(0, 1_000_000_000L, 2_000_001L),
        AudioTimestampMapper.RejectionReason.INVALID_UNCERTAINTY,
        "invalid uncertainty");

    mapper.observe(100, 1_000_000_000L, 1_000L);
    checkReason(
        mapper.observe(100, 1_100_000_000L, 1_000L),
        AudioTimestampMapper.RejectionReason.NONMONOTONIC_FRAME_POSITION,
        "repeated frame");
    checkReason(
        mapper.observe(200, 999_999_999L, 1_000L),
        AudioTimestampMapper.RejectionReason.NONMONOTONIC_BOOTTIME,
        "backward time");
  }

  private static void boundsRetainedEvidenceAndExtrapolation() {
    AudioTimestampMapper mapper = new AudioTimestampMapper(config(4));
    for (int second = 0; second <= 5; ++second) {
      check(
          mapper.observe(
                  second * 48_000L,
                  20_000_000_000L + second * 1_000_000_000L,
                  1_000L)
              .accepted(),
          "bounded evidence point " + second);
    }

    check(mapper.retainedEvidenceCount() == 4, "evidence ring is bounded");
    check(mapper.estimateBoottime(0).isEmpty(), "old extrapolation rejected");
    check(mapper.estimateBoottime(2 * 48_000L).isPresent(), "oldest retained anchor maps");
    check(mapper.estimateBoottime(5 * 48_000L + 24_000).isPresent(), "boundary extrapolation maps");
    check(
        mapper.estimateBoottime(5 * 48_000L + 24_001).isEmpty(),
        "beyond extrapolation rejected");
  }

  private static void checkReason(
      AudioTimestampMapper.ObservationResult result,
      AudioTimestampMapper.RejectionReason expected,
      String message) {
    check(!result.accepted(), message + " should be rejected");
    check(result.rejectionReason() == expected, message + " rejection reason");
  }

  private static void checkNear(double actual, double expected, double tolerance, String message) {
    check(Math.abs(actual - expected) <= tolerance,
        message + ": expected " + expected + ", got " + actual);
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
