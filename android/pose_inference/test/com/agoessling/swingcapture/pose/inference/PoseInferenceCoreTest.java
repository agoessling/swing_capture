package com.agoessling.swingcapture.pose.inference;

import com.agoessling.swingcapture.pose.PoseLandmarkFrame;
import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;

public final class PoseInferenceCoreTest {
  public static void main(String[] args) {
    convertsI420Deterministically();
    convertsStrideAwareYuvInOnePass();
    convertsStrideAwareYuvIntoReusableAlignedRgbStorage();
    gatesAtFiveHertzAndRejectsClockRegression();
    keepsOnlyLatestPendingFrame();
    defersOwnedInferenceCloseUntilActiveWorkFinishes();
    boundsAndSerializesTrace();
  }

  private static void convertsI420Deterministically() {
    I420Frame frame =
        new I420Frame(
            3,
            1,
            42,
            new byte[] {16, (byte) 235, 81},
            new byte[] {(byte) 128, 90},
            new byte[] {(byte) 128, (byte) 240});
    RgbFrame result = I420ToRgb.convert(frame);
    check(result.width() == 3 && result.height() == 1, "dimensions");
    check(result.timestampNs() == 42, "timestamp");
    check(result.argb()[0] == 0xff000000, "black conversion");
    check(result.argb()[1] == 0xffffffff, "white conversion");
    check((result.argb()[2] & 0x00ff0000) == 0x00ff0000, "red conversion");
    check((result.argb()[2] & 0x0000ffff) == 0, "red channel isolation");
  }

  private static void convertsStrideAwareYuvInOnePass() {
    ByteBuffer y = ByteBuffer.wrap(new byte[] {99, 16, (byte) 235, 99, 99, 81, (byte) 145, 99});
    ByteBuffer u = ByteBuffer.wrap(new byte[] {99, (byte) 128, 99});
    ByteBuffer v = ByteBuffer.wrap(new byte[] {99, (byte) 128, 99});
    RgbFrame result =
        StrideAwareYuv420ToRgb.convert(
            2,
            2,
            42,
            new StrideAwareYuv420ToRgb.Plane(y, 1, 4, 1),
            new StrideAwareYuv420ToRgb.Plane(u, 1, 3, 1),
            new StrideAwareYuv420ToRgb.Plane(v, 1, 3, 1));
    check(result.timestampNs() == 42, "stride-aware timestamp");
    check(result.argb()[0] == 0xff000000, "stride-aware black");
    check(result.argb()[1] == 0xffffffff, "stride-aware white");
    check(result.argb()[2] == 0xff4c4c4c, "stride-aware dark gray");
    check(result.argb()[3] == 0xff969696, "stride-aware light gray");
  }

  private static void convertsStrideAwareYuvIntoReusableAlignedRgbStorage() {
    ByteBuffer y = ByteBuffer.wrap(new byte[] {99, 16, (byte) 235, 99, 99, 81, (byte) 145, 99});
    ByteBuffer u = ByteBuffer.wrap(new byte[] {99, (byte) 128, 99});
    ByteBuffer v = ByteBuffer.wrap(new byte[] {99, (byte) 128, 99});
    y.position(1);
    u.position(1);
    v.position(1);
    check(StrideAwareYuv420ToRgb.rgbBufferSize(2, 2) == 16, "aligned RGB capacity");
    check(StrideAwareYuv420ToRgb.rgbBufferSize(640, 360) == 691_200, "640x360 RGB capacity");
    ByteBuffer rgb = ByteBuffer.allocateDirect(16);
    StrideAwareYuv420ToRgb.convertToRgb(
        2,
        2,
        new StrideAwareYuv420ToRgb.Plane(y, 1, 4, 1),
        new StrideAwareYuv420ToRgb.Plane(u, 1, 3, 1),
        new StrideAwareYuv420ToRgb.Plane(v, 1, 3, 1),
        rgb);
    check(rgb.position() == 0 && rgb.limit() == 16, "RGB buffer bounds");
    checkRgb(rgb, 0, 0, 0, 0, "RGB black");
    checkRgb(rgb, 3, 255, 255, 255, "RGB white");
    check((rgb.get(6) | rgb.get(7)) == 0, "first-row alignment padding");
    checkRgb(rgb, 8, 76, 76, 76, "RGB dark gray");
    checkRgb(rgb, 11, 150, 150, 150, "RGB light gray");
    check((rgb.get(14) | rgb.get(15)) == 0, "second-row alignment padding");
    check(y.position() == 1 && u.position() == 1 && v.position() == 1, "input positions unchanged");

    y.put(1, (byte) 235);
    StrideAwareYuv420ToRgb.convertToRgb(
        2,
        2,
        new StrideAwareYuv420ToRgb.Plane(y, 1, 4, 1),
        new StrideAwareYuv420ToRgb.Plane(u, 1, 3, 1),
        new StrideAwareYuv420ToRgb.Plane(v, 1, 3, 1),
        rgb);
    checkRgb(rgb, 0, 255, 255, 255, "reused RGB storage");
    expectThrows(
        IllegalArgumentException.class,
        () ->
            StrideAwareYuv420ToRgb.convertToRgb(
                2,
                2,
                new StrideAwareYuv420ToRgb.Plane(y, 1, 4, 1),
                new StrideAwareYuv420ToRgb.Plane(u, 1, 3, 1),
                new StrideAwareYuv420ToRgb.Plane(v, 1, 3, 1),
                ByteBuffer.allocate(16)));
    expectThrows(
        IllegalArgumentException.class,
        () ->
            StrideAwareYuv420ToRgb.convertToRgb(
                2,
                2,
                new StrideAwareYuv420ToRgb.Plane(y, 1, 4, 1),
                new StrideAwareYuv420ToRgb.Plane(u, 1, 3, 1),
                new StrideAwareYuv420ToRgb.Plane(v, 1, 3, 1),
                ByteBuffer.allocateDirect(15)));
    expectThrows(
        IllegalArgumentException.class,
        () ->
            StrideAwareYuv420ToRgb.convertToRgb(
                2,
                2,
                new StrideAwareYuv420ToRgb.Plane(y, 1, 4, 1),
                new StrideAwareYuv420ToRgb.Plane(u, 1, 3, 1),
                new StrideAwareYuv420ToRgb.Plane(v, 1, 3, 1),
                ByteBuffer.allocateDirect(17)));
  }

  private static void checkRgb(
      ByteBuffer buffer,
      int offset,
      int red,
      int green,
      int blue,
      String message) {
    check((buffer.get(offset) & 0xff) == red, message + " red");
    check((buffer.get(offset + 1) & 0xff) == green, message + " green");
    check((buffer.get(offset + 2) & 0xff) == blue, message + " blue");
  }

  private static void gatesAtFiveHertzAndRejectsClockRegression() {
    FiveHertzFrameGate gate = new FiveHertzFrameGate();
    check(gate.accept(0), "first frame");
    check(!gate.accept(199_999_999L), "sub-interval frame");
    check(gate.accept(200_000_000L), "five-hertz interval");
    expectThrows(IllegalArgumentException.class, () -> gate.accept(199_000_000L));
  }

  private static void keepsOnlyLatestPendingFrame() {
    ManualExecutor executor = new ManualExecutor();
    FakeInference inference = new FakeInference();
    List<Long> results = new ArrayList<>();
    PoseInferenceMetrics metrics = new PoseInferenceMetrics();
    PoseInferenceTrace trace = new PoseInferenceTrace(8);
    SequenceClock clock = new SequenceClock(10, 20, 30, 50);
    PoseInferencePipeline pipeline =
        new PoseInferencePipeline(
            executor, inference, result -> results.add(result.timestampNs()), metrics, trace, clock);

    check(pipeline.submit(frame(0)), "first scheduled");
    check(!pipeline.submit(frame(100_000_000L)), "cadence filtered");
    check(pipeline.submit(frame(200_000_000L)), "second retained");
    check(pipeline.submit(frame(400_000_000L)), "newest retained");
    check(executor.size() == 1, "only active work queued");
    executor.runNext();
    check(executor.size() == 1, "latest frame scheduled after active frame");
    executor.runNext();

    check(results.equals(List.of(0L, 400_000_000L)), "latest-only results");
    PoseInferenceMetrics.Snapshot snapshot = metrics.snapshot();
    check(snapshot.offered() == 4, "offered count");
    check(snapshot.cadenceRejected() == 1, "cadence count");
    check(snapshot.scheduled() == 3, "scheduled count");
    check(snapshot.droppedBackpressure() == 1, "drop count");
    check(snapshot.succeeded() == 2 && snapshot.failed() == 0, "completion counts");
    check(snapshot.totalInferenceNs() == 30 && snapshot.maxInferenceNs() == 20, "timing");
    check(
        snapshot
            .toJson()
            .equals(
                "{\"offered\":4,\"cadence_rejected\":1,\"scheduled\":3,"
                    + "\"dropped_backpressure\":1,\"succeeded\":2,\"failed\":0,"
                    + "\"total_inference_ns\":30,\"max_inference_ns\":20}"),
        "metrics serialization");
    check(trace.snapshot().size() == 2, "trace count");
    pipeline.close();
    check(inference.closeCount == 1, "inference closed");
  }

  private static void defersOwnedInferenceCloseUntilActiveWorkFinishes() {
    ManualExecutor executor = new ManualExecutor();
    FakeInference inference = new FakeInference();
    PoseInferenceMetrics metrics = new PoseInferenceMetrics();
    PoseInferencePipeline pipeline =
        new PoseInferencePipeline(
            executor,
            inference,
            unused -> {},
            metrics,
            new PoseInferenceTrace(2),
            new SequenceClock(1, 2));
    pipeline.submit(frame(0));
    pipeline.submit(frame(200_000_000L));
    pipeline.close();
    pipeline.close();
    check(inference.closeCount == 0, "active inference remains open");
    check(metrics.snapshot().droppedBackpressure() == 1, "pending frame dropped on close");
    executor.runNext();
    check(inference.closeCount == 1, "inference closes after active work");
    check(!pipeline.submit(frame(400_000_000L)), "submit rejected after close");
  }

  private static void boundsAndSerializesTrace() {
    PoseInferenceTrace trace = new PoseInferenceTrace(1);
    trace.add(new PoseInferenceTrace.Event(1, 2, 3, PoseInferenceDelegate.CPU, "old", 0));
    trace.add(new PoseInferenceTrace.Event(4, 5, 6, PoseInferenceDelegate.GPU, "ok", 1));
    check(trace.snapshot().size() == 1, "trace bounded");
    check(
        trace.toNdjson().equals(
            "{\"frame_timestamp_ns\":4,\"started_ns\":5,\"finished_ns\":6,"
                + "\"delegate\":\"gpu\",\"outcome\":\"ok\",\"pose_count\":1}\n"),
        "trace serialization");
  }

  private static RgbFrame frame(long timestampNs) {
    return new RgbFrame(1, 1, timestampNs, new int[] {0xff000000});
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }

  private static void expectThrows(Class<? extends Throwable> type, Runnable action) {
    try {
      action.run();
    } catch (Throwable exception) {
      if (type.isInstance(exception)) {
        return;
      }
      throw new AssertionError("wrong exception", exception);
    }
    throw new AssertionError("expected " + type.getSimpleName());
  }

  private static final class ManualExecutor implements java.util.concurrent.Executor {
    private final ArrayDeque<Runnable> tasks = new ArrayDeque<>();

    @Override
    public void execute(Runnable task) {
      tasks.addLast(task);
    }

    int size() {
      return tasks.size();
    }

    void runNext() {
      tasks.removeFirst().run();
    }
  }

  private static final class FakeInference implements PoseFrameInference {
    int closeCount;

    @Override
    public PoseInferenceDelegate actualDelegate() {
      return PoseInferenceDelegate.CPU;
    }

    @Override
    public PoseLandmarkFrame infer(RgbFrame frame) {
      return new PoseLandmarkFrame(frame.timestampNs(), 0.0, java.util.Map.of());
    }

    @Override
    public void close() {
      closeCount++;
    }
  }

  private static final class SequenceClock implements PoseInferencePipeline.NanoClock {
    private final ArrayDeque<Long> values = new ArrayDeque<>();

    SequenceClock(long... values) {
      for (long value : values) {
        this.values.addLast(value);
      }
    }

    @Override
    public long nanoTime() {
      return values.removeFirst();
    }
  }
}
