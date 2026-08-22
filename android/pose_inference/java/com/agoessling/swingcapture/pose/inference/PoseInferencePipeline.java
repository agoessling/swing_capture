package com.agoessling.swingcapture.pose.inference;

import com.agoessling.swingcapture.pose.PoseLandmarkFrame;
import java.util.Objects;
import java.util.concurrent.Executor;
import java.util.function.Consumer;

/** Five-Hz, latest-frame-wins inference scheduler with bounded memory and explicit lifecycle. */
public final class PoseInferencePipeline implements AutoCloseable {
  @FunctionalInterface
  public interface NanoClock {
    long nanoTime();
  }

  private final Executor executor;
  private final PoseFrameInference inference;
  private final Consumer<PoseLandmarkFrame> resultConsumer;
  private final NanoClock clock;
  private final FiveHertzFrameGate frameGate = new FiveHertzFrameGate();
  private final PoseInferenceMetrics metrics;
  private final PoseInferenceTrace trace;

  private RgbFrame pending;
  private boolean running;
  private boolean closeRequested;
  private boolean inferenceClosed;

  public PoseInferencePipeline(
      Executor executor,
      PoseFrameInference inference,
      Consumer<PoseLandmarkFrame> resultConsumer,
      PoseInferenceMetrics metrics,
      PoseInferenceTrace trace) {
    this(executor, inference, resultConsumer, metrics, trace, System::nanoTime);
  }

  public PoseInferencePipeline(
      Executor executor,
      PoseFrameInference inference,
      Consumer<PoseLandmarkFrame> resultConsumer,
      PoseInferenceMetrics metrics,
      PoseInferenceTrace trace,
      NanoClock clock) {
    this.executor = Objects.requireNonNull(executor, "executor");
    this.inference = Objects.requireNonNull(inference, "inference");
    this.resultConsumer = Objects.requireNonNull(resultConsumer, "resultConsumer");
    this.metrics = Objects.requireNonNull(metrics, "metrics");
    this.trace = Objects.requireNonNull(trace, "trace");
    this.clock = Objects.requireNonNull(clock, "clock");
  }

  /** Returns false when cadence-filtered or after close; true when retained for processing. */
  public synchronized boolean submit(RgbFrame frame) {
    Objects.requireNonNull(frame, "frame");
    metrics.recordOffered();
    if (closeRequested) {
      return false;
    }
    if (!frameGate.accept(frame.timestampNs())) {
      metrics.recordCadenceRejected();
      return false;
    }
    metrics.recordScheduled();
    if (running) {
      if (pending != null) {
        metrics.recordDroppedBackpressure();
      }
      pending = frame;
      return true;
    }
    running = true;
    execute(frame);
    return true;
  }

  public PoseInferenceMetrics metrics() {
    return metrics;
  }

  public PoseInferenceTrace trace() {
    return trace;
  }

  @Override
  public synchronized void close() {
    if (closeRequested) {
      return;
    }
    closeRequested = true;
    if (pending != null) {
      pending = null;
      metrics.recordDroppedBackpressure();
    }
    if (!running) {
      closeInference();
    }
  }

  private void execute(RgbFrame frame) {
    try {
      executor.execute(() -> process(frame));
    } catch (RuntimeException exception) {
      running = false;
      metrics.recordCompleted(0, false);
      closeInferenceIfRequested();
      throw exception;
    }
  }

  private void process(RgbFrame frame) {
    long startedNs = clock.nanoTime();
    boolean success = false;
    int poseCount = 0;
    String outcome = "ok";
    try {
      PoseLandmarkFrame result = inference.infer(frame);
      poseCount = result.landmarks().isEmpty() ? 0 : 1;
      resultConsumer.accept(result);
      success = true;
    } catch (RuntimeException exception) {
      outcome = exception.getClass().getSimpleName();
    }
    long finishedNs = clock.nanoTime();
    metrics.recordCompleted(Math.max(0, finishedNs - startedNs), success);
    trace.add(
        new PoseInferenceTrace.Event(
            frame.timestampNs(),
            startedNs,
            finishedNs,
            inference.actualDelegate(),
            outcome,
            poseCount));

    synchronized (this) {
      if (!closeRequested && pending != null) {
        RgbFrame next = pending;
        pending = null;
        execute(next);
      } else {
        running = false;
        closeInferenceIfRequested();
      }
    }
  }

  private void closeInferenceIfRequested() {
    if (closeRequested) {
      closeInference();
    }
  }

  private void closeInference() {
    if (!inferenceClosed) {
      inferenceClosed = true;
      inference.close();
    }
  }
}
