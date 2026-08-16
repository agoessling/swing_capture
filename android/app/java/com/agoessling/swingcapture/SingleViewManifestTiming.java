package com.agoessling.swingcapture;

/** Platform-neutral timing fields for a manifest containing exactly one camera view. */
public record SingleViewManifestTiming(
    Long mappedNearestFrameSkewUs, long localNearestFrameResidualUs) {
  public static final String INTER_VIEW_SKEW_FIELD = "mapped_nearest_frame_skew_us";
  public static final String LOCAL_RESIDUAL_FIELD = "local_nearest_frame_residual_us";

  public SingleViewManifestTiming {
    if (mappedNearestFrameSkewUs != null) {
      throw new IllegalArgumentException("A single-view manifest cannot claim inter-view skew");
    }
    if (localNearestFrameResidualUs < 0) {
      throw new IllegalArgumentException("Local nearest-frame residual cannot be negative");
    }
  }

  public static SingleViewManifestTiming fromAbsoluteDeltaUs(long absoluteDeltaUs) {
    return new SingleViewManifestTiming(null, absoluteDeltaUs);
  }
}
