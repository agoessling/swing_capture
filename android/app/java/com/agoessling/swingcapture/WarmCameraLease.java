package com.agoessling.swingcapture;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraConstrainedHighSpeedCaptureSession;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.TotalCaptureResult;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.Range;
import android.util.Size;
import android.view.Surface;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

/** One rear CameraDevice that moves from a scheduled 5 Hz YUV session into 720p240 encoding. */
public final class WarmCameraLease implements AutoCloseable {
  private static final int HIGH_SPEED_FPS = 240;
  private static final int STANDBY_WIDTH = 640;
  private static final int STANDBY_HEIGHT = 360;
  private static final long STANDBY_INTERVAL_MILLIS = 200;
  private static final long START_TIMEOUT_SECONDS = 6;
  private static final long STOP_TIMEOUT_MILLIS = 3_000;

  /** Ownership of each non-null image transfers to the listener, which must close it. */
  @FunctionalInterface
  public interface StandbyImageListener {
    void onImage(Image image);

    default void onFailure(Throwable failure) {}
  }

  /** High-speed capture evidence consumed by the encoded-retention timestamp calibrator. */
  public interface HighSpeedListener {
    void onCaptureStarted(long elapsedRealtimeNanos);

    void onCaptureCompleted(long frameNumber, long sensorTimestampNanos);

    void onFailure(Throwable failure);
  }

  private record Selection(String cameraId, int timestampSource, Size standbySize) {}

  private final CameraManager manager;
  private final CaptureProfile profile;
  private final Selection selection;
  private final StandbyImageListener imageListener;
  private final HandlerThread cameraThread = new HandlerThread("pose-warm-camera");
  private final AtomicReference<Throwable> failure = new AtomicReference<>();
  private final AtomicBoolean transitioning = new AtomicBoolean();
  private final AtomicBoolean closed = new AtomicBoolean();
  private final CountDownLatch deviceOpened = new CountDownLatch(1);
  private final CountDownLatch standbyConfigured = new CountDownLatch(1);
  private final CountDownLatch firstStandbyFrame = new CountDownLatch(1);
  private final CountDownLatch standbyClosed = new CountDownLatch(1);
  private Handler handler;
  private ImageReader imageReader;
  private CameraDevice device;
  private CameraCaptureSession standbySession;
  private CameraConstrainedHighSpeedCaptureSession highSpeedSession;

  private WarmCameraLease(
      Context context,
      CaptureProfile profile,
      Selection selection,
      StandbyImageListener imageListener) {
    manager = context.getSystemService(CameraManager.class);
    this.profile = Objects.requireNonNull(profile, "profile");
    this.selection = Objects.requireNonNull(selection, "selection");
    this.imageListener = Objects.requireNonNull(imageListener, "imageListener");
  }

  public static WarmCameraLease open(
      Context context, CaptureProfile profile, StandbyImageListener imageListener)
      throws Exception {
    Context application = context.getApplicationContext();
    if (application.checkSelfPermission(Manifest.permission.CAMERA)
        != PackageManager.PERMISSION_GRANTED) {
      throw new IllegalStateException("Camera permission is required for pose standby");
    }
    Selection selection = selectCamera(application, profile);
    WarmCameraLease lease =
        new WarmCameraLease(application, profile, selection, imageListener);
    try {
      lease.startStandby();
      return lease;
    } catch (Throwable failure) {
      try {
        lease.close();
      } catch (Throwable closeFailure) {
        failure.addSuppressed(closeFailure);
      }
      if (failure instanceof Exception exception) {
        throw exception;
      }
      throw new IllegalStateException("Unable to start pose standby camera", failure);
    }
  }

  public String cameraId() {
    return selection.cameraId();
  }

  public int timestampSource() {
    return selection.timestampSource();
  }

  public Size standbySize() {
    return selection.standbySize();
  }

  /** Replaces the standby session without reopening the camera and waits for its first frame. */
  public void transitionToHighSpeed(Surface encoderSurface, HighSpeedListener listener)
      throws Exception {
    Objects.requireNonNull(encoderSurface, "encoderSurface");
    Objects.requireNonNull(listener, "listener");
    if (!transitioning.compareAndSet(false, true)) {
      throw new IllegalStateException("camera lease is already transitioning");
    }
    CountDownLatch configured = new CountDownLatch(1);
    CountDownLatch firstFrame = new CountDownLatch(1);
    CameraCaptureSession standby = standbySession;
    if (standby == null) {
      throw new IllegalStateException("standby session is unavailable");
    }
    try {
      standby.abortCaptures();
    } finally {
      standby.close();
    }
    await(standbyClosed, "standby session close");
    ImageReader reader = imageReader;
    imageReader = null;
    if (reader != null) {
      reader.close();
    }
    createHighSpeedSession(encoderSurface, listener, configured, firstFrame);
    await(configured, "high-speed session configuration");
    await(firstFrame, "first high-speed frame");
    throwIfFailed();
  }

  private void startStandby() throws Exception {
    cameraThread.start();
    handler = new Handler(cameraThread.getLooper());
    Size size = selection.standbySize();
    imageReader =
        // The application retains at most one running plus one pending image. A third HAL slot is
        // still required so acquireLatestImage() can acquire-and-drop while those two are open.
        ImageReader.newInstance(size.getWidth(), size.getHeight(), ImageFormat.YUV_420_888, 3);
    imageReader.setOnImageAvailableListener(
        reader -> {
          Image image = null;
          try {
            image = reader.acquireLatestImage();
            if (image != null) {
              imageListener.onImage(image);
              image = null;
            }
          } catch (Throwable callbackFailure) {
            fail(callbackFailure);
          } finally {
            if (image != null) {
              image.close();
            }
          }
        },
        handler);
    manager.openCamera(
        selection.cameraId(),
        new CameraDevice.StateCallback() {
          @Override
          public void onOpened(CameraDevice opened) {
            device = opened;
            deviceOpened.countDown();
            createStandbySession(opened);
          }

          @Override
          public void onDisconnected(CameraDevice disconnected) {
            disconnected.close();
            fail(new IllegalStateException("Pose standby camera disconnected"));
          }

          @Override
          public void onError(CameraDevice errored, int errorCode) {
            errored.close();
            fail(new IllegalStateException("Pose standby camera error " + errorCode));
          }
        },
        handler);
    await(deviceOpened, "camera open");
    await(standbyConfigured, "standby session configuration");
    await(firstStandbyFrame, "first standby frame");
  }

  @SuppressWarnings("deprecation")
  private void createStandbySession(CameraDevice opened) {
    try {
      Surface surface = imageReader.getSurface();
      opened.createCaptureSession(
          Collections.singletonList(surface),
          new CameraCaptureSession.StateCallback() {
            @Override
            public void onConfigured(CameraCaptureSession configured) {
              standbySession = configured;
              standbyConfigured.countDown();
              try {
                CaptureRequest.Builder builder =
                    opened.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                builder.addTarget(surface);
                builder.set(
                    CaptureRequest.CONTROL_AF_MODE,
                    CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE);
                submitStandbyCapture(configured, builder.build());
              } catch (Throwable configureFailure) {
                fail(configureFailure);
              }
            }

            @Override
            public void onConfigureFailed(CameraCaptureSession ignored) {
              fail(new IllegalStateException("Pose standby session configuration failed"));
            }

            @Override
            public void onClosed(CameraCaptureSession ignored) {
              standbyClosed.countDown();
            }
          },
          handler);
    } catch (Throwable configureFailure) {
      fail(configureFailure);
    }
  }

  private void submitStandbyCapture(CameraCaptureSession session, CaptureRequest request) {
    if (transitioning.get() || closed.get() || failure.get() != null) {
      return;
    }
    try {
      long submittedNs = SystemClock.elapsedRealtimeNanos();
      session.capture(
          request,
          new CameraCaptureSession.CaptureCallback() {
            @Override
            public void onCaptureCompleted(
                CameraCaptureSession ignoredSession,
                CaptureRequest ignoredRequest,
                TotalCaptureResult ignoredResult) {
              firstStandbyFrame.countDown();
              if (WarmCaptureTransitionTiming.standbyCaptureFailureIsFatal(
                  transitioning.get(), closed.get())) {
                long delayMillis =
                    LowRateCaptureSchedule.nextDelayMillis(
                        submittedNs,
                        SystemClock.elapsedRealtimeNanos(),
                        STANDBY_INTERVAL_MILLIS);
                handler.postDelayed(() -> submitStandbyCapture(session, request), delayMillis);
              }
            }

            @Override
            public void onCaptureFailed(
                CameraCaptureSession ignoredSession,
                CaptureRequest ignoredRequest,
                android.hardware.camera2.CaptureFailure captureFailure) {
              if (!transitioning.get() && !closed.get()) {
                fail(
                    new IllegalStateException(
                        "Pose standby capture failed: " + captureFailure.getReason()));
              }
            }
          },
          handler);
    } catch (Throwable captureFailure) {
      fail(captureFailure);
    }
  }

  @SuppressWarnings("deprecation")
  private void createHighSpeedSession(
      Surface encoderSurface,
      HighSpeedListener listener,
      CountDownLatch configured,
      CountDownLatch firstFrame) {
    try {
      CameraDevice current = device;
      if (current == null) {
        throw new IllegalStateException("camera device is unavailable during transition");
      }
      CaptureRequest.Builder builder =
          current.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
      builder.addTarget(encoderSurface);
      builder.set(
          CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE,
          new Range<>(HIGH_SPEED_FPS, HIGH_SPEED_FPS));
      builder.set(
          CaptureRequest.CONTROL_AF_MODE,
          CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
      current.createConstrainedHighSpeedCaptureSession(
          Collections.singletonList(encoderSurface),
          new CameraCaptureSession.StateCallback() {
            @Override
            public void onConfigured(CameraCaptureSession session) {
              highSpeedSession = (CameraConstrainedHighSpeedCaptureSession) session;
              configured.countDown();
              try {
                List<CaptureRequest> burst =
                    highSpeedSession.createHighSpeedRequestList(builder.build());
                AtomicLong firstHighSpeedFrameNumber = new AtomicLong(-1L);
                highSpeedSession.setRepeatingBurst(
                    burst,
                    new CameraCaptureSession.CaptureCallback() {
                      @Override
                      public void onCaptureStarted(
                          CameraCaptureSession ignoredSession,
                          CaptureRequest ignoredRequest,
                          long captureTimestamp,
                          long frameNumber) {
                        firstHighSpeedFrameNumber.compareAndSet(-1L, frameNumber);
                        listener.onCaptureStarted(captureTimestamp);
                        firstFrame.countDown();
                      }

                      @Override
                      public void onCaptureCompleted(
                          CameraCaptureSession ignoredSession,
                          CaptureRequest ignoredRequest,
                          TotalCaptureResult result) {
                        Long timestamp = result.get(CaptureResult.SENSOR_TIMESTAMP);
                        if (timestamp == null) {
                          listener.onFailure(
                              new IllegalStateException(
                                  "High-speed result omitted SENSOR_TIMESTAMP"));
                          return;
                        }
                        // A constrained high-speed burst can report onCaptureStarted only once per
                        // eight-request batch on Pixel devices. TotalCaptureResult is delivered per
                        // encoded frame, so use its exact SENSOR_TIMESTAMP and normalize the
                        // device-global frame number into this high-speed session's ordinal.
                        listener.onCaptureCompleted(
                            WarmCaptureTransitionTiming.highSpeedOrdinal(
                                firstHighSpeedFrameNumber.get(), result.getFrameNumber()),
                            timestamp);
                      }

                      @Override
                      public void onCaptureFailed(
                          CameraCaptureSession ignoredSession,
                          CaptureRequest ignoredRequest,
                          android.hardware.camera2.CaptureFailure captureFailure) {
                        listener.onFailure(
                            new IllegalStateException(
                                "High-speed capture failed: " + captureFailure.getReason()));
                      }
                    },
                    handler);
              } catch (Throwable startFailure) {
                listener.onFailure(startFailure);
                fail(startFailure);
              }
            }

            @Override
            public void onConfigureFailed(CameraCaptureSession ignored) {
              Throwable configureFailure =
                  new IllegalStateException("High-speed session configuration failed");
              listener.onFailure(configureFailure);
              fail(configureFailure);
            }
          },
          handler);
    } catch (Throwable configureFailure) {
      listener.onFailure(configureFailure);
      fail(configureFailure);
    }
  }

  @Override
  public void close() {
    if (!closed.compareAndSet(false, true)) {
      return;
    }
    transitioning.set(true);
    if (highSpeedSession != null) {
      try {
        highSpeedSession.stopRepeating();
        highSpeedSession.abortCaptures();
      } catch (Throwable stopFailure) {
        failure.compareAndSet(null, stopFailure);
      }
      highSpeedSession.close();
    }
    if (standbySession != null) {
      standbySession.close();
    }
    if (device != null) {
      device.close();
    }
    if (imageReader != null) {
      imageReader.close();
    }
    if (cameraThread.isAlive()) {
      cameraThread.quitSafely();
      try {
        cameraThread.join(STOP_TIMEOUT_MILLIS);
      } catch (InterruptedException interrupted) {
        Thread.currentThread().interrupt();
        failure.compareAndSet(null, interrupted);
      }
      if (cameraThread.isAlive()) {
        failure.compareAndSet(
            null, new IllegalStateException("Pose camera handler thread did not stop"));
      }
    }
  }

  private void await(CountDownLatch latch, String operation) throws Exception {
    if (!latch.await(START_TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
      throwIfFailed();
      throw new IllegalStateException("Timed out waiting for " + operation);
    }
    throwIfFailed();
  }

  private void fail(Throwable cameraFailure) {
    boolean firstFailure = failure.compareAndSet(null, cameraFailure);
    deviceOpened.countDown();
    standbyConfigured.countDown();
    firstStandbyFrame.countDown();
    standbyClosed.countDown();
    if (firstFailure) {
      try {
        imageListener.onFailure(cameraFailure);
      } catch (RuntimeException ignored) {
        // Preserve the originating camera failure.
      }
    }
  }

  private void throwIfFailed() {
    Throwable current = failure.get();
    if (current != null) {
      throw new IllegalStateException("Warm camera lease failed", current);
    }
  }

  private static Selection selectCamera(Context context, CaptureProfile profile) throws Exception {
    CameraManager manager = context.getSystemService(CameraManager.class);
    for (String cameraId : manager.getCameraIdList()) {
      CameraCharacteristics characteristics = manager.getCameraCharacteristics(cameraId);
      Integer facing = characteristics.get(CameraCharacteristics.LENS_FACING);
      Integer timestampSource =
          characteristics.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE);
      StreamConfigurationMap streams =
          characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
      if (facing == null
          || facing != CameraCharacteristics.LENS_FACING_BACK
          || timestampSource == null
          || timestampSource != CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME
          || streams == null) {
        continue;
      }
      Size highSpeedSize = new Size(profile.width(), profile.height());
      if (!Arrays.asList(streams.getHighSpeedVideoSizes()).contains(highSpeedSize)
          || Arrays.stream(streams.getHighSpeedVideoFpsRangesFor(highSpeedSize))
              .noneMatch(range -> range.getLower() == HIGH_SPEED_FPS
                  && range.getUpper() == HIGH_SPEED_FPS)) {
        continue;
      }
      return new Selection(
          cameraId,
          timestampSource,
          chooseStandbySize(streams, profile.width() / (double) profile.height()));
    }
    throw new IllegalStateException(
        "No realtime rear camera supports the selected 240 fps profile and YUV standby");
  }

  private static Size chooseStandbySize(StreamConfigurationMap streams, double targetAspect) {
    Size[] sizes = streams.getOutputSizes(ImageFormat.YUV_420_888);
    if (sizes == null || sizes.length == 0) {
      throw new IllegalStateException("Camera does not expose a YUV standby output");
    }
    Comparator<Size> byDistance =
        Comparator.<Size>comparingLong(
                size ->
                    Math.abs((long) size.getWidth() * size.getHeight()
                        - (long) STANDBY_WIDTH * STANDBY_HEIGHT))
            .thenComparingLong(size -> (long) size.getWidth() * size.getHeight());
    return Arrays.stream(sizes)
        .filter(size -> size.getWidth() >= STANDBY_WIDTH && size.getHeight() >= STANDBY_HEIGHT)
        .filter(
            size -> Math.abs(size.getWidth() / (double) size.getHeight() - targetAspect) <= 0.03)
        .min(byDistance)
        .orElseThrow(
            () -> new IllegalStateException("Camera has no 16:9 YUV standby size at least 640x360"));
  }
}
