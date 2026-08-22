package com.agoessling.swingcapture;

import android.content.Context;
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.graphics.YuvImage;
import android.media.Image;
import android.os.SystemClock;
import android.util.Size;
import com.agoessling.swingcapture.diagnostics.PreviewEvidence;
import com.agoessling.swingcapture.diagnostics.PreviewEvidenceRing;
import com.agoessling.swingcapture.pose.NormalizedHittingRegion;
import com.agoessling.swingcapture.pose.PoseLandmarkFrame;
import com.agoessling.swingcapture.pose.PoseLandmarkObservationExtractor;
import com.agoessling.swingcapture.pose.PoseProjection;
import com.agoessling.swingcapture.pose.PoseTriggerController;
import com.agoessling.swingcapture.pose.inference.MediaPipePoseLandmarker;
import com.agoessling.swingcapture.pose.inference.PoseInferenceDelegate;
import com.agoessling.swingcapture.pose.inference.PoseInferenceDelegatePolicy;
import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.util.Locale;
import java.util.Objects;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

/**
 * Five-Hz Camera2-to-MediaPipe standby pipeline that can hand its still-open camera to 240 fps.
 *
 * <p>Camera Images are strictly bounded to one running inference and one latest pending image. The
 * inference call synchronously converts the Camera2 Image into MediaPipe's reusable direct RGB
 * input buffer. Debug JPEGs are optional, copied at a reduced cadence, and encoded by a separate
 * latest-only worker. Only the arm-decision JPEG receives a bounded flush within the existing
 * transfer deadline; evidence work never retains a Camera2 Image.
 */
public final class PoseStandbyEngine implements AutoCloseable {
  private static final long DEFAULT_TRANSFER_TIMEOUT_MILLIS = 3_000;
  private static final int DEFAULT_JPEG_QUALITY = 70;

  public record Config(
      CaptureProfile captureProfile,
      CaptureRole captureRole,
      PoseInferenceDelegatePolicy delegatePolicy,
      NormalizedHittingRegion hittingRegion,
      PoseLandmarkObservationExtractor.Config featureConfig,
      PoseTriggerController.Config controllerConfig,
      boolean debugEvidenceEnabled,
      int imageRotationDegrees,
      int jpegQuality,
      long transferTimeoutMillis) {
    public Config {
      Objects.requireNonNull(captureProfile, "captureProfile");
      Objects.requireNonNull(captureRole, "captureRole");
      Objects.requireNonNull(delegatePolicy, "delegatePolicy");
      Objects.requireNonNull(hittingRegion, "hittingRegion");
      Objects.requireNonNull(featureConfig, "featureConfig");
      Objects.requireNonNull(controllerConfig, "controllerConfig");
      PoseStandbyPolicy.projectionForRole(captureRole);
      if (imageRotationDegrees != 0
          && imageRotationDegrees != 90
          && imageRotationDegrees != 180
          && imageRotationDegrees != 270) {
        throw new IllegalArgumentException("imageRotationDegrees must be 0, 90, 180, or 270");
      }
      if (jpegQuality < 1 || jpegQuality > 100) {
        throw new IllegalArgumentException("jpegQuality must be in [1, 100]");
      }
      if (transferTimeoutMillis <= 0) {
        throw new IllegalArgumentException("transferTimeoutMillis must be positive");
      }
    }

    public static Config defaults(
        CaptureProfile captureProfile,
        CaptureRole captureRole,
        PoseInferenceDelegatePolicy delegatePolicy,
        NormalizedHittingRegion hittingRegion,
        boolean debugEvidenceEnabled) {
      return new Config(
          captureProfile,
          captureRole,
          delegatePolicy,
          hittingRegion,
          PoseLandmarkObservationExtractor.Config.defaultsForFiveFramesPerSecond(),
          PoseTriggerController.Config.defaultsForFiveFramesPerSecond(),
          debugEvidenceEnabled,
          0,
          DEFAULT_JPEG_QUALITY,
          DEFAULT_TRANSFER_TIMEOUT_MILLIS);
    }
  }

  public record ReadyStatus(
      String cameraId,
      int timestampSource,
      Size standbySize,
      PoseInferenceDelegate inferenceDelegate,
      PoseProjection projection) {
    public ReadyStatus {
      Objects.requireNonNull(cameraId, "cameraId");
      Objects.requireNonNull(standbySize, "standbySize");
      Objects.requireNonNull(inferenceDelegate, "inferenceDelegate");
      Objects.requireNonNull(projection, "projection");
    }
  }

  /** Callbacks are serialized on a dedicated event thread, never on the inference worker. */
  public interface Listener {
    default void onReady(ReadyStatus status) {}

    default void onDecision(
        PoseLandmarkObservationExtractor.Evaluation evaluation,
        PoseTriggerController.Decision decision) {}

    default void onFailure(Throwable failure) {}
  }

  private enum Lifecycle {
    STARTING,
    RUNNING,
    TRANSFERRING,
    TRANSFERRED,
    STOPPING,
    CLOSED
  }

  private record EvidenceWork(
      byte[] nv21,
      int width,
      int height,
      long timestampNs,
      boolean armFrame) {}

  private final Object lifecycleLock = new Object();
  private final Config config;
  private final Listener listener;
  private final PoseProjection projection;
  private final MediaPipePoseLandmarker landmarker;
  private final PoseTriggerController controller;
  private final PoseStandbyMetrics metrics;
  private final PreviewEvidenceRing evidenceRing;
  private final LatestFrameSlot<Image> frameSlot = new LatestFrameSlot<>();
  private final LatestFrameSlot<EvidenceWork> evidenceSlot = new LatestFrameSlot<>();
  private final ArmEvidenceFlush armEvidenceFlush = new ArmEvidenceFlush();
  private final ExecutorService inferenceExecutor;
  private final ExecutorService evidenceExecutor;
  private final ExecutorService eventExecutor;
  private final DebugEvidenceCadence evidenceCadence =
      new DebugEvidenceCadence(DebugEvidenceCadence.DEFAULT_INTERVAL_NANOS);
  private Lifecycle lifecycle = Lifecycle.STARTING;
  private WarmCameraLease cameraLease;
  private PoseLandmarkFrame previousLandmarkFrame;
  private boolean landmarkerClosed;

  private PoseStandbyEngine(
      Config config,
      Listener listener,
      MediaPipePoseLandmarker landmarker,
      PoseTriggerController controller) {
    this.config = Objects.requireNonNull(config, "config");
    this.listener = Objects.requireNonNull(listener, "listener");
    this.landmarker = Objects.requireNonNull(landmarker, "landmarker");
    projection = PoseStandbyPolicy.projectionForRole(config.captureRole());
    this.controller = Objects.requireNonNull(controller, "controller");
    metrics = new PoseStandbyMetrics(landmarker.actualDelegate());
    evidenceRing =
        new PreviewEvidenceRing(
            PreviewEvidenceRing.RECOMMENDED_RETENTION_NANOS,
            PreviewEvidenceRing.RECOMMENDED_MAXIMUM_ENTRY_COUNT,
            PreviewEvidenceRing.RECOMMENDED_MAXIMUM_COMPRESSED_BYTES);
    inferenceExecutor =
        Executors.newSingleThreadExecutor(namedThreadFactory("pose-standby-inference"));
    evidenceExecutor =
        config.debugEvidenceEnabled()
            ? Executors.newSingleThreadExecutor(namedThreadFactory("pose-standby-evidence"))
            : null;
    eventExecutor = Executors.newSingleThreadExecutor(namedThreadFactory("pose-standby-events"));
  }

  /** Opens the model and camera synchronously; callbacks begin only after standby is ready. */
  public static PoseStandbyEngine start(Context context, Config config, Listener listener)
      throws Exception {
    return start(
        context,
        config,
        new PoseTriggerController(config.controllerConfig()),
        listener);
  }

  /**
   * Opens standby around an existing controller whose hysteresis must survive a capture cycle.
   *
   * <p>The caller must stop the previous engine before reusing the controller. This overload lets
   * the foreground service preserve {@code WAITING_FOR_CLEAR} across the temporary 240 fps session
   * instead of treating the golfer's finish pose as a fresh address candidate.
   */
  static PoseStandbyEngine start(
      Context context,
      Config config,
      PoseTriggerController controller,
      Listener listener)
      throws Exception {
    Objects.requireNonNull(context, "context");
    Objects.requireNonNull(config, "config");
    Objects.requireNonNull(controller, "controller");
    Objects.requireNonNull(listener, "listener");
    MediaPipePoseLandmarker landmarker =
        MediaPipePoseLandmarker.open(context.getApplicationContext(), config.delegatePolicy());
    PoseStandbyEngine engine = new PoseStandbyEngine(config, listener, landmarker, controller);
    try {
      WarmCameraLease opened =
          WarmCameraLease.open(
              context.getApplicationContext(),
              config.captureProfile(),
              new WarmCameraLease.StandbyImageListener() {
                @Override
                public void onImage(Image image) {
                  engine.offerImage(image);
                }

                @Override
                public void onFailure(Throwable failure) {
                  engine.reportFailure(failure);
                }
              });
      synchronized (engine.lifecycleLock) {
        engine.cameraLease = opened;
        engine.lifecycle = Lifecycle.RUNNING;
      }
      engine.dispatch(
          () ->
              listener.onReady(
                  new ReadyStatus(
                      opened.cameraId(),
                      opened.timestampSource(),
                      opened.standbySize(),
                      landmarker.actualDelegate(),
                      engine.projection)));
      return engine;
    } catch (Throwable failure) {
      engine.close();
      if (failure instanceof Exception exception) {
        throw exception;
      }
      throw new IllegalStateException("Unable to start pose standby", failure);
    }
  }

  public PoseStandbyMetrics.Snapshot metrics() {
    return metrics.snapshot();
  }

  public PreviewEvidenceRing.Snapshot previewEvidenceSnapshot() {
    return evidenceRing.snapshot();
  }

  public PoseTriggerController.State controllerState() {
    synchronized (controller) {
      return controller.state();
    }
  }

  /** Commits the candidate arm JPEG after service-level precedence accepts the transition. */
  void acceptArmEvidence(long timestampNs) {
    if (evidenceExecutor != null) {
      armEvidenceFlush.accept(timestampNs);
    }
  }

  /** Releases a candidate arm JPEG when service-level diagnostic precedence vetoes the arm. */
  void cancelArmEvidence(long timestampNs) {
    if (evidenceExecutor != null) {
      armEvidenceFlush.cancel(timestampNs);
    }
  }

  /**
   * Stops inference and detaches the still-open warm camera for a constrained high-speed session.
   *
   * <p>If inference does not finish within the configured bound, no lease is detached and callers
   * may retry or close the engine. A successful transfer makes subsequent {@link #close()} calls
   * leave the returned lease untouched; its new owner must close it.
   */
  public synchronized WarmCameraLease transferToHighSpeed()
      throws InterruptedException, TimeoutException {
    long transferDeadlineNs =
        saturatedAdd(
            System.nanoTime(), TimeUnit.MILLISECONDS.toNanos(config.transferTimeoutMillis()));
    synchronized (lifecycleLock) {
      if (lifecycle != Lifecycle.RUNNING && lifecycle != Lifecycle.TRANSFERRING) {
        throw new IllegalStateException("pose standby is not available for transfer");
      }
      lifecycle = Lifecycle.TRANSFERRING;
    }
    closeDropped(frameSlot.stopAcceptingAndTakePending());
    if (!frameSlot.awaitIdle(remainingNanos(transferDeadlineNs), TimeUnit.NANOSECONDS)) {
      throw new TimeoutException("Timed out waiting for pose inference to release its Camera2 Image");
    }
    closeLandmarker();
    inferenceExecutor.shutdown();
    flushArmEvidence(transferDeadlineNs);

    WarmCameraLease transferred;
    synchronized (lifecycleLock) {
      transferred = cameraLease;
      if (transferred == null) {
        throw new IllegalStateException("warm camera lease is unavailable");
      }
      cameraLease = null;
      lifecycle = Lifecycle.TRANSFERRED;
    }
    eventExecutor.shutdown();
    return transferred;
  }

  @Override
  public void close() {
    WarmCameraLease leaseToClose;
    synchronized (lifecycleLock) {
      if (lifecycle == Lifecycle.CLOSED) {
        return;
      }
      if (lifecycle == Lifecycle.TRANSFERRED) {
        lifecycle = Lifecycle.CLOSED;
        inferenceExecutor.shutdownNow();
        stopEvidenceWorker(true);
        eventExecutor.shutdown();
        return;
      }
      lifecycle = Lifecycle.STOPPING;
      leaseToClose = cameraLease;
      cameraLease = null;
    }

    closeDropped(frameSlot.stopAcceptingAndTakePending());
    boolean interrupted = false;
    boolean inferenceIdle = false;
    try {
      inferenceIdle = frameSlot.awaitIdle(config.transferTimeoutMillis(), TimeUnit.MILLISECONDS);
      if (!inferenceIdle) {
        inferenceExecutor.shutdownNow();
      }
    } catch (InterruptedException exception) {
      interrupted = true;
      inferenceExecutor.shutdownNow();
    }
    stopEvidenceWorker(true);
    if (inferenceIdle) {
      closeLandmarker();
      inferenceExecutor.shutdownNow();
      if (leaseToClose != null) {
        leaseToClose.close();
      }
    } else {
      deferResourceCloseUntilInferenceStops(leaseToClose);
    }
    eventExecutor.shutdown();
    synchronized (lifecycleLock) {
      lifecycle = Lifecycle.CLOSED;
    }
    if (interrupted) {
      Thread.currentThread().interrupt();
    }
  }

  private void offerImage(Image image) {
    Objects.requireNonNull(image, "image");
    metrics.recordOffered();
    synchronized (lifecycleLock) {
      if (lifecycle != Lifecycle.RUNNING) {
        metrics.recordDropped();
        image.close();
        return;
      }
    }
    LatestFrameSlot.Offer<Image> offer = frameSlot.offer(image);
    if (!offer.accepted()) {
      metrics.recordDropped();
      offer.dropped().close();
      return;
    }
    metrics.recordScheduled();
    closeDropped(offer.dropped());
    if (offer.scheduleNow() != null) {
      scheduleDrain(offer.scheduleNow());
    }
  }

  private void scheduleDrain(Image first) {
    try {
      inferenceExecutor.execute(() -> drainFrames(first));
    } catch (RejectedExecutionException rejected) {
      metrics.recordDropped();
      first.close();
      Image next = frameSlot.completeAndTakeNext();
      while (next != null) {
        closeDropped(next);
        next = frameSlot.completeAndTakeNext();
      }
      reportFailure(rejected);
    }
  }

  private void drainFrames(Image first) {
    Image current = first;
    while (current != null) {
      boolean terminalFailure = false;
      try {
        processImage(current);
      } catch (Throwable failure) {
        terminalFailure = true;
        reportFailure(failure);
      } finally {
        current.close();
      }
      if (terminalFailure) {
        closeDropped(frameSlot.stopAcceptingAndTakePending());
        closeDropped(frameSlot.completeAndTakeNext());
        return;
      }
      current = frameSlot.completeAndTakeNext();
    }
  }

  private void processImage(Image image) {
    long startedNs = SystemClock.elapsedRealtimeNanos();
    PoseLandmarkObservationExtractor.Evaluation evaluation;
    PoseTriggerController.Decision decision;
    long inferenceDurationNs;
    try {
      long imageTimestampNs = image.getTimestamp();
      PoseLandmarkFrame landmarkFrame = inferYuvImage(image, imageTimestampNs);
      inferenceDurationNs =
          Math.max(0, SystemClock.elapsedRealtimeNanos() - startedNs);
      evaluation =
          PoseLandmarkObservationExtractor.evaluate(
              landmarkFrame,
              previousLandmarkFrame,
              config.hittingRegion(),
              config.featureConfig(),
              projection);
      synchronized (controller) {
        decision = controller.observe(evaluation.toControllerObservation());
      }
      previousLandmarkFrame = landmarkFrame;
      metrics.recordInference(inferenceDurationNs, true);
    } catch (RuntimeException failure) {
      long durationNs = Math.max(0, SystemClock.elapsedRealtimeNanos() - startedNs);
      metrics.recordInference(durationNs, false);
      reportFailure(failure);
      return;
    }

    if (config.debugEvidenceEnabled()) {
      appendObservationEvidence(inferenceDurationNs, evaluation, decision);
      boolean armDecision = decision.command() == PoseTriggerController.Command.START_HIGH_SPEED;
      if (evidenceCadence.shouldSnapshot(
              evaluation.timestampNs(), armDecision)
          && (armDecision || armEvidenceFlush.permitsOrdinaryWork())) {
        offerDebugEvidence(image, evaluation, armDecision);
      }
    }
    // Register the arm JPEG before exposing START_HIGH_SPEED to the service. Transfer can now
    // distinguish that priority work from ordinary best-effort 1 Hz evidence.
    dispatch(() -> listener.onDecision(evaluation, decision));
  }

  private void appendObservationEvidence(
      long inferenceDurationNs,
      PoseLandmarkObservationExtractor.Evaluation evaluation,
      PoseTriggerController.Decision decision) {
    try {
      evidenceRing.append(
          new PreviewEvidence(
              evaluation.timestampNs(),
              new byte[0],
              modelAndDelegate(),
              inferenceDurationNs,
              evaluation.personConfidence(),
              evaluation.addressConfidence(),
              evaluation.motionMagnitude(),
              evaluation.insideHittingRegion(),
              PoseStandbyPolicy.evidenceState(decision.state()),
              decision.reason()));
      metrics.recordObservation(true);
    } catch (RuntimeException rejected) {
      metrics.recordObservation(false);
    }
  }

  private PoseLandmarkFrame inferYuvImage(Image image, long imageTimestampNs) {
    if (image.getFormat() != ImageFormat.YUV_420_888 || image.getPlanes().length != 3) {
      throw new IllegalArgumentException("pose inference requires a three-plane YUV_420_888 Image");
    }
    return landmarker.infer(image, imageTimestampNs, config.imageRotationDegrees());
  }

  private void offerDebugEvidence(
      Image image,
      PoseLandmarkObservationExtractor.Evaluation evaluation,
      boolean armFrame) {
    if (armFrame) {
      armEvidenceFlush.request(evaluation.timestampNs());
    }
    try {
      Image.Plane[] planes = image.getPlanes();
      byte[] nv21 =
          Yuv420PlanePacker.toNv21(
              image.getWidth(),
              image.getHeight(),
              viewPlane(planes[0]),
              viewPlane(planes[1]),
              viewPlane(planes[2]));
      EvidenceWork work =
          new EvidenceWork(
              nv21,
              image.getWidth(),
              image.getHeight(),
              evaluation.timestampNs(),
              armFrame);
      metrics.recordEvidenceOffered();
      LatestFrameSlot.Offer<EvidenceWork> offer = evidenceSlot.offer(work);
      if (!offer.accepted()) {
        recordEvidenceDropped(work);
        return;
      }
      if (offer.dropped() != null) {
        recordEvidenceDropped(offer.dropped());
      }
      if (offer.scheduleNow() != null) {
        scheduleEvidenceDrain(offer.scheduleNow());
      }
    } catch (RuntimeException failure) {
      metrics.recordEvidence(false);
      if (armFrame) {
        armEvidenceFlush.complete(evaluation.timestampNs(), false);
      }
    }
  }

  private void scheduleEvidenceDrain(EvidenceWork first) {
    try {
      evidenceExecutor.execute(() -> drainEvidence(first));
    } catch (RejectedExecutionException rejected) {
      recordEvidenceDropped(first);
      EvidenceWork next = evidenceSlot.completeAndTakeNext();
      while (next != null) {
        recordEvidenceDropped(next);
        next = evidenceSlot.completeAndTakeNext();
      }
    }
  }

  private void drainEvidence(EvidenceWork first) {
    EvidenceWork current = first;
    while (current != null) {
      appendDebugEvidence(current);
      current = evidenceSlot.completeAndTakeNext();
    }
  }

  private void appendDebugEvidence(EvidenceWork work) {
    boolean present = false;
    try {
      byte[] jpeg = encodeJpeg(work.nv21(), work.width(), work.height(), config.jpegQuality());
      if (evidenceRing.attachCompressedFrame(work.timestampNs(), jpeg)) {
        metrics.recordEvidence(true);
        present = true;
      } else {
        metrics.recordEvidenceDropped();
      }
    } catch (RuntimeException failure) {
      // Evidence is deliberately best-effort and must never disable pose arming.
      metrics.recordEvidence(false);
    } finally {
      if (work.armFrame()) {
        armEvidenceFlush.complete(work.timestampNs(), present);
      }
    }
  }

  private void recordEvidenceDropped(EvidenceWork work) {
    metrics.recordEvidenceDropped();
    if (work.armFrame()) {
      armEvidenceFlush.complete(work.timestampNs(), false);
    }
  }

  private static byte[] encodeJpeg(byte[] nv21, int width, int height, int quality) {
    YuvImage yuv = new YuvImage(nv21, ImageFormat.NV21, width, height, null);
    ByteArrayOutputStream output =
        new ByteArrayOutputStream(Math.max(1_024, width * height / 4));
    if (!yuv.compressToJpeg(new Rect(0, 0, width, height), quality, output)) {
      throw new IllegalStateException("YUV preview JPEG encoding failed");
    }
    return output.toByteArray();
  }

  private static Yuv420PlanePacker.Plane viewPlane(Image.Plane plane) {
    ByteBuffer buffer = plane.getBuffer().duplicate();
    return new Yuv420PlanePacker.Plane(
        buffer, buffer.position(), plane.getRowStride(), plane.getPixelStride());
  }

  private String modelAndDelegate() {
    return MediaPipePoseLandmarker.MODEL_ASSET_PATH
        + ":"
        + landmarker.actualDelegate().name().toLowerCase(Locale.ROOT);
  }

  private void closeDropped(Image image) {
    if (image != null) {
      metrics.recordDropped();
      image.close();
    }
  }

  private void closeLandmarker() {
    synchronized (lifecycleLock) {
      if (landmarkerClosed) {
        return;
      }
      landmarkerClosed = true;
    }
    landmarker.close();
  }

  private void stopEvidenceWorker(boolean interrupt) {
    if (evidenceExecutor == null) {
      return;
    }
    EvidenceWork pending = evidenceSlot.stopAcceptingAndTakePending();
    if (pending != null) {
      recordEvidenceDropped(pending);
    }
    if (interrupt) {
      evidenceExecutor.shutdownNow();
    } else {
      evidenceExecutor.shutdown();
    }
  }

  private void flushArmEvidence(long transferDeadlineNs) throws InterruptedException {
    if (evidenceExecutor == null || !armEvidenceFlush.accepted()) {
      stopEvidenceWorker(false);
      return;
    }
    evidenceExecutor.shutdown();
    ArmEvidenceFlush.Outcome outcome;
    try {
      outcome =
          armEvidenceFlush.await(
              remainingNanos(transferDeadlineNs), TimeUnit.NANOSECONDS);
    } catch (InterruptedException interrupted) {
      stopEvidenceWorker(true);
      throw interrupted;
    }
    metrics.recordArmEvidenceFlush(outcome);
    if (outcome == ArmEvidenceFlush.Outcome.PRESENT) {
      stopEvidenceWorker(false);
      return;
    }
    // FAILED and TIMED_OUT preserve the synchronous five-Hz observation row. The explicit
    // timeout policy keeps optional JPEG compression from missing the backswing.
    ArmEvidenceFlush.timeoutPolicy();
    stopEvidenceWorker(true);
  }

  private static long remainingNanos(long deadlineNs) {
    return Math.max(0, deadlineNs - System.nanoTime());
  }

  private static long saturatedAdd(long left, long right) {
    if (right > 0 && left > Long.MAX_VALUE - right) {
      return Long.MAX_VALUE;
    }
    return left + right;
  }

  private void deferResourceCloseUntilInferenceStops(WarmCameraLease leaseToClose) {
    Thread cleanup =
        namedThreadFactory("pose-standby-deferred-cleanup")
            .newThread(
                () -> {
                  boolean interrupted = false;
                  while (true) {
                    try {
                      if (inferenceExecutor.awaitTermination(1, TimeUnit.DAYS)) {
                        break;
                      }
                    } catch (InterruptedException waitInterrupted) {
                      interrupted = true;
                    }
                  }
                  closeLandmarker();
                  if (leaseToClose != null) {
                    leaseToClose.close();
                  }
                  if (interrupted) {
                    Thread.currentThread().interrupt();
                  }
                });
    cleanup.start();
  }

  private void reportFailure(Throwable failure) {
    dispatch(() -> listener.onFailure(failure));
  }

  private void dispatch(Runnable callback) {
    try {
      eventExecutor.execute(
          () -> {
            try {
              callback.run();
            } catch (RuntimeException ignored) {
              // A consumer callback must not kill the serialized event executor.
            }
          });
    } catch (RejectedExecutionException ignored) {
      // Shutdown intentionally stops future callbacks.
    }
  }

  private static ThreadFactory namedThreadFactory(String name) {
    return runnable -> {
      Thread thread = new Thread(runnable, name);
      thread.setDaemon(true);
      return thread;
    };
  }
}
