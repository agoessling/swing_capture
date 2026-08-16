package com.agoessling.swingcapture.core.coordination;

/** Deterministic golden cases for four-timestamp offset and RTT bounds. */
public final class ClockExchangeEstimatorTest {
  private ClockExchangeEstimatorTest() {}

  public static void main(String[] args) {
    estimatesIntersectedBounds();
    requiresRepeatedSamples();
    rejectsInconsistentBounds();
    validatesExchangeOrdering();
  }

  private static void estimatesIntersectedBounds() {
    ClockExchangeEstimator estimator = new ClockExchangeEstimator("face-node");
    estimator.add(new ClockExchangeSample(10_000, 11_200, 11_250, 10_650));
    estimator.add(new ClockExchangeSample(20_000, 21_100, 21_150, 20_250));
    estimator.add(new ClockExchangeSample(30_000, 31_150, 31_200, 30_250));

    ClockExchangeEstimateResult result = estimator.estimate();
    check(result.status() == ClockExchangeEstimateStatus.READY, "estimate should be ready");
    ClockOffsetEstimate estimate = result.estimate().orElseThrow();
    check(estimate.offsetNs() == 1_025, "intersected offset midpoint");
    check(estimate.uncertaintyNs() == 75, "intersected offset uncertainty");
    check(estimate.offsetLowerBoundNs() == 950, "offset lower bound");
    check(estimate.offsetUpperBoundNs() == 1_100, "offset upper bound");
    check(estimate.minimumRoundTripNs() == 200, "minimum network RTT");
    check(estimate.maximumRoundTripNs() == 600, "maximum network RTT");
    check(estimate.sampleCount() == 3, "sample count");

    NodeTriggerReport report =
        new NodeTriggerReport(CaptureRole.FACE_ON, "face-node", "swing-1", 101_025, 25);
    ObservedNodeTrigger observed = ObservedNodeTrigger.map(report, estimate, 100_500);
    check(observed.coordinatorTimestampNs() == 100_000, "mapped coordinator timestamp");
    check(observed.coordinatorUncertaintyNs() == 100, "composed timestamp uncertainty");
  }

  private static void requiresRepeatedSamples() {
    ClockExchangeEstimator estimator = new ClockExchangeEstimator("node");
    estimator.add(new ClockExchangeSample(1_000, 2_050, 2_060, 1_110));
    check(
        estimator.estimate().status() == ClockExchangeEstimateStatus.INSUFFICIENT_SAMPLES,
        "a single exchange must not claim readiness");
  }

  private static void rejectsInconsistentBounds() {
    ClockExchangeEstimator estimator = new ClockExchangeEstimator("node", 2);
    estimator.add(new ClockExchangeSample(1_000, 2_100, 2_100, 1_200));
    estimator.add(new ClockExchangeSample(2_000, 3_400, 3_400, 2_200));
    check(
        estimator.estimate().status() == ClockExchangeEstimateStatus.INCONSISTENT_BOUNDS,
        "nonintersecting bounds must be explicit");
  }

  private static void validatesExchangeOrdering() {
    expectFailure(
        () -> new ClockExchangeSample(100, 1_000, 1_200, 200),
        "processing time larger than total exchange must fail");
    ClockExchangeEstimator estimator = new ClockExchangeEstimator("node", 2);
    estimator.add(new ClockExchangeSample(100, 1_050, 1_060, 210));
    expectFailure(
        () -> estimator.add(new ClockExchangeSample(110, 1_060, 1_070, 205)),
        "receive timestamps must increase");
  }

  private static void expectFailure(Runnable operation, String message) {
    try {
      operation.run();
    } catch (IllegalArgumentException expected) {
      return;
    }
    throw new AssertionError(message);
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
