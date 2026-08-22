package com.agoessling.swingcapture.pose.inference;

/** Host coverage for bounded, deterministic replay decoder performance telemetry. */
public final class MediaCodecReplayFrameSourceContractTest {
  private MediaCodecReplayFrameSourceContractTest() {}

  public static void main(String[] arguments) {
    serializesStableBoundedMetrics();
    rejectsContradictoryMetrics();
  }

  private static void serializesStableBoundedMetrics() {
    MediaCodecReplayFrameSource.PerformanceSnapshot snapshot =
        new MediaCodecReplayFrameSource.PerformanceSnapshot(
            "c2.test.avc.decoder",
            "image_reader_surface",
            7,
            241,
            235,
            6,
            1_200_000_000L,
            3_000_000L,
            90_000_000L,
            1_500_000_000L);

    check(
        snapshot
            .toLogString()
            .equals(
                "decoder=c2.test.avc.decoder output_transport=image_reader_surface"
                    + " next_frame_calls=7 output_buffers=241"
                    + " skipped_output_buffers=235 selected_frames=6 dequeue_ms=1200"
                    + " image_wait_ms=3 conversion_ms=90 next_frame_ms=1500"),
        "stable decoder metric serialization");
  }

  private static void rejectsContradictoryMetrics() {
    expectThrows(
        IllegalArgumentException.class,
        () ->
            new MediaCodecReplayFrameSource.PerformanceSnapshot(
                "c2.test.avc.decoder", "image_reader_surface", 1, 2, 3, 1, 0, 0, 0, 0));
    expectThrows(
        IllegalArgumentException.class,
        () ->
            new MediaCodecReplayFrameSource.PerformanceSnapshot(
                "c2.test.avc.decoder", "image_reader_surface", 1, 2, 1, 3, 0, 0, 0, 0));
  }

  private static void expectThrows(Class<? extends Throwable> type, Runnable action) {
    try {
      action.run();
    } catch (Throwable thrown) {
      if (type.isInstance(thrown)) {
        return;
      }
      throw new AssertionError("wrong exception", thrown);
    }
    throw new AssertionError("expected " + type.getSimpleName());
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
