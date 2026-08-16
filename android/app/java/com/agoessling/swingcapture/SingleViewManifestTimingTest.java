package com.agoessling.swingcapture;

/** Deterministic host tests for single-view manifest timing semantics. */
public final class SingleViewManifestTimingTest {
  private SingleViewManifestTimingTest() {}

  public static void main(String[] args) {
    check(
        SingleViewManifestTiming.INTER_VIEW_SKEW_FIELD.equals("mapped_nearest_frame_skew_us"),
        "platform inter-view field name");
    check(
        SingleViewManifestTiming.LOCAL_RESIDUAL_FIELD.equals("local_nearest_frame_residual_us"),
        "Android local residual field name");
    SingleViewManifestTiming timing = SingleViewManifestTiming.fromAbsoluteDeltaUs(1_939);
    check(timing.mappedNearestFrameSkewUs() == null, "single view must not claim inter-view skew");
    check(
        timing.localNearestFrameResidualUs() == 1_939,
        "local trigger-to-frame residual is preserved");
    expectFailure(
        () -> new SingleViewManifestTiming(0L, 1), "non-null inter-view skew must be rejected");
    expectFailure(
        () -> SingleViewManifestTiming.fromAbsoluteDeltaUs(-1),
        "negative absolute delta must be rejected");
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
