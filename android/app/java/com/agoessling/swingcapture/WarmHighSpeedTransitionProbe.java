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
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.Range;
import android.util.Size;
import android.view.Surface;
import java.nio.ByteBuffer;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONArray;
import org.json.JSONObject;

/** Measures a low-rate warm Camera2 session transitioning to constrained 240 fps capture. */
public final class WarmHighSpeedTransitionProbe {
  private static final int STANDBY_FRAMES = 6;
  private static final long STANDBY_FRAME_INTERVAL_MILLIS = 200;
  private static final long START_TIMEOUT_SECONDS = 5;
  private static final long STOP_TIMEOUT_SECONDS = 3;
  private static final long MAXIMUM_TRANSITION_TO_CAMERA_NS = TimeUnit.SECONDS.toNanos(2);
  private static final long MAXIMUM_TRANSITION_TO_ENCODER_NS = TimeUnit.MILLISECONDS.toNanos(2500);
  private static final long MAXIMUM_HIGH_SPEED_GAP_NS = TimeUnit.MICROSECONDS.toNanos(6250);
  private static final long STARTUP_CADENCE_DISCARD_NS = TimeUnit.MILLISECONDS.toNanos(250);

  private WarmHighSpeedTransitionProbe() {}

  public static JSONObject run(
      Context context,
      CaptureConfigurationSnapshot captureConfiguration,
      HighSpeedProbe.Request request)
      throws Exception {
    captureConfiguration.requireAssignedRole();
    JSONObject report = new JSONObject();
    report.put("schema_version", 1);
    report.put("report_type", "android_warm_high_speed_transition");
    report.put("complete", true);
    report.put("created_at_utc", Instant.now().toString());
    report.put("node_id", captureConfiguration.nodeId());
    report.put("role", captureConfiguration.role().wireName());
    report.put("request", requestJson(request));
    report.put("standby_target_frames_per_second", 5);
    report.put("thermal_status_before", thermalStatus(context));

    WarmCamera camera = null;
    PreparedEncoder encoder = null;
    CameraSelection selection = null;
    Throwable fatalFailure = null;
    try {
      requirePermission(context, Manifest.permission.CAMERA);
      selection = selectCamera(context, request);
      report.put("camera_id", selection.cameraId());
      report.put("camera_timestamp_source", selection.timestampSource());

      encoder = new PreparedEncoder(request);
      encoder.prepare();
      camera = new WarmCamera(context, selection.cameraId(), selection.standbySize(), request);
      camera.startStandby();
      camera.transitionToHighSpeed(encoder);
      Thread.sleep(request.durationMillis());
    } catch (Throwable failure) {
      fatalFailure = failure;
    } finally {
      if (camera != null) {
        try {
          camera.stop();
        } catch (Throwable stopFailure) {
          camera.recordFailure(stopFailure);
        }
      }
      if (encoder != null) {
        try {
          encoder.stop();
        } catch (Throwable stopFailure) {
          encoder.recordFailure(stopFailure);
        }
      }
    }

    if (camera != null) {
      report.put("standby", camera.standbyJson());
      report.put("high_speed_camera", camera.highSpeedJson(encoder));
    }
    if (encoder != null) {
      report.put("encoder", encoder.toJson());
    }
    if (fatalFailure != null) {
      report.put("fatal_error", describe(fatalFailure));
    }
    report.put("thermal_status_after", thermalStatus(context));

    JSONArray failures = acceptanceFailures(camera, encoder, fatalFailure);
    report.put("acceptance_failures", failures);
    report.put("passed", failures.length() == 0);
    return report;
  }

  private static JSONObject requestJson(HighSpeedProbe.Request request) throws Exception {
    return new JSONObject()
        .put("width", request.width())
        .put("height", request.height())
        .put("frames_per_second", request.framesPerSecond())
        .put("duration_ms", request.durationMillis())
        .put("bitrate_bits_per_second", request.bitrateBitsPerSecond())
        .put("mime", request.mime());
  }

  private static JSONArray acceptanceFailures(
      WarmCamera camera, PreparedEncoder encoder, Throwable fatalFailure) {
    JSONArray failures = new JSONArray();
    if (fatalFailure != null) {
      failures.put("fatal_error");
    }
    if (camera == null) {
      failures.put("camera_not_created");
      return failures;
    }
    if (camera.failure() != null) {
      failures.put("camera_failure");
    }
    if (camera.cameraOpenCount() != 1) {
      failures.put("camera_was_not_opened_exactly_once");
    }
    List<Long> standby = camera.standbySensorTimestamps();
    if (standby.size() < STANDBY_FRAMES) {
      failures.put("insufficient_low_rate_standby_frames");
    } else {
      long elapsed = standby.get(standby.size() - 1) - standby.get(0);
      double rate = (standby.size() - 1) * 1_000_000_000.0 / (double) elapsed;
      if (rate < 4.0 || rate > 6.0) {
        failures.put("standby_frame_rate_out_of_range");
      }
    }
    List<Long> highSpeed =
        stableTail(camera.highSpeedSensorTimestamps(), STARTUP_CADENCE_DISCARD_NS);
    if (!hasMonotonicCameraResults(highSpeed)) {
      failures.put("high_speed_camera_results_invalid");
    }
    if (encoder == null || encoder.failure() != null) {
      failures.put("encoder_failure");
    } else if (!hasExpectedEncoderCadence(
        stableTail(
            encoder.presentationTimestampsUs(),
            TimeUnit.NANOSECONDS.toMicros(STARTUP_CADENCE_DISCARD_NS)))) {
      failures.put("encoder_cadence_invalid");
    }
    try {
      WarmCaptureTransitionTiming timing = camera.transitionTiming(encoder);
      if (timing.transitionToFirstHighSpeedCameraFrameNs() > MAXIMUM_TRANSITION_TO_CAMERA_NS) {
        failures.put("warm_transition_to_camera_exceeded_two_seconds");
      }
      if (timing.transitionToFirstUsableEncodedFrameNs() > MAXIMUM_TRANSITION_TO_ENCODER_NS) {
        failures.put("warm_transition_to_encoder_exceeded_2500_milliseconds");
      }
    } catch (RuntimeException invalidTiming) {
      failures.put("transition_timing_invalid");
    }
    return failures;
  }

  private static boolean hasMonotonicCameraResults(List<Long> timestampsNs) {
    if (timestampsNs.size() < 30) {
      return false;
    }
    for (int index = 1; index < timestampsNs.size(); ++index) {
      if (timestampsNs.get(index) <= timestampsNs.get(index - 1)) {
        return false;
      }
    }
    return true;
  }

  private static boolean hasExpectedEncoderCadence(List<Long> timestampsUs) {
    if (timestampsUs.size() < 200) {
      return false;
    }
    long elapsed = timestampsUs.get(timestampsUs.size() - 1) - timestampsUs.get(0);
    if (elapsed <= 0) {
      return false;
    }
    double framesPerSecond = (timestampsUs.size() - 1) * 1_000_000.0 / (double) elapsed;
    return framesPerSecond >= 235.0
        && framesPerSecond <= 245.0
        && maximumGap(timestampsUs) <= TimeUnit.NANOSECONDS.toMicros(MAXIMUM_HIGH_SPEED_GAP_NS);
  }

  private static long maximumGap(List<Long> timestamps) {
    long maximum = 0;
    for (int index = 1; index < timestamps.size(); ++index) {
      maximum = Math.max(maximum, timestamps.get(index) - timestamps.get(index - 1));
    }
    return maximum;
  }

  private static List<Long> stableTail(List<Long> timestamps, long discardedDuration) {
    if (timestamps.isEmpty()) {
      return Collections.emptyList();
    }
    long threshold = Math.addExact(timestamps.get(0), discardedDuration);
    return timestamps.stream().filter(timestamp -> timestamp >= threshold).toList();
  }

  private static void requirePermission(Context context, String permission) {
    if (context.checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) {
      throw new IllegalStateException("Required permission is not granted: " + permission);
    }
  }

  private static int thermalStatus(Context context) {
    return context.getSystemService(PowerManager.class).getCurrentThermalStatus();
  }

  private static CameraSelection selectCamera(Context context, HighSpeedProbe.Request request)
      throws Exception {
    CameraManager manager = context.getSystemService(CameraManager.class);
    for (String cameraId : manager.getCameraIdList()) {
      CameraCharacteristics characteristics = manager.getCameraCharacteristics(cameraId);
      Integer facing = characteristics.get(CameraCharacteristics.LENS_FACING);
      if (facing == null || facing != CameraCharacteristics.LENS_FACING_BACK) {
        continue;
      }
      Integer timestampSource =
          characteristics.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE);
      if (timestampSource == null
          || timestampSource != CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME) {
        continue;
      }
      StreamConfigurationMap streams =
          characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
      if (streams == null) {
        continue;
      }
      Size requestedSize = new Size(request.width(), request.height());
      if (!Arrays.asList(streams.getHighSpeedVideoSizes()).contains(requestedSize)) {
        continue;
      }
      boolean fixedRate = false;
      for (Range<Integer> range : streams.getHighSpeedVideoFpsRangesFor(requestedSize)) {
        if (range.getLower() == request.framesPerSecond()
            && range.getUpper() == request.framesPerSecond()) {
          fixedRate = true;
          break;
        }
      }
      if (!fixedRate) {
        continue;
      }
      Size standby = chooseStandbySize(streams, request.width() / (double) request.height());
      return new CameraSelection(cameraId, timestampSource, standby);
    }
    throw new IllegalStateException(
        "No realtime rear camera supports "
            + request.width()
            + "x"
            + request.height()
            + " at fixed "
            + request.framesPerSecond()
            + " fps with a low-rate YUV standby surface");
  }

  private static Size chooseStandbySize(StreamConfigurationMap streams, double targetAspect) {
    Size[] sizes = streams.getOutputSizes(ImageFormat.YUV_420_888);
    if (sizes == null || sizes.length == 0) {
      throw new IllegalStateException("Camera does not expose a YUV standby output");
    }
    Comparator<Size> byArea =
        Comparator.comparingLong(size -> (long) size.getWidth() * size.getHeight());
    List<Size> usable =
        Arrays.stream(sizes)
            .filter(size -> size.getWidth() >= 320 && size.getHeight() >= 180)
            .toList();
    if (usable.isEmpty()) {
      throw new IllegalStateException("Camera has no usable YUV standby size");
    }
    return usable.stream()
        .filter(
            size ->
                Math.abs(size.getWidth() / (double) size.getHeight() - targetAspect) <= 0.03)
        .min(byArea)
        .orElseGet(() -> usable.stream().min(byArea).orElseThrow());
  }

  private static String selectEncoder(HighSpeedProbe.Request request) {
    for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.ALL_CODECS).getCodecInfos()) {
      if (!info.isEncoder() || !info.isHardwareAccelerated()) {
        continue;
      }
      for (String supportedType : info.getSupportedTypes()) {
        if (!request.mime().equalsIgnoreCase(supportedType)) {
          continue;
        }
        try {
          if (info
              .getCapabilitiesForType(supportedType)
              .getVideoCapabilities()
              .areSizeAndRateSupported(
                  request.width(), request.height(), request.framesPerSecond())) {
            return info.getName();
          }
        } catch (IllegalArgumentException ignored) {
          // Continue to the next hardware encoder.
        }
      }
    }
    throw new IllegalStateException("No suitable hardware high-speed encoder is available");
  }

  private static JSONObject timingJson(List<Long> timestamps, double unitsPerSecond)
      throws Exception {
    JSONObject json = new JSONObject();
    json.put("count", timestamps.size());
    if (timestamps.isEmpty()) {
      json.put("first", JSONObject.NULL);
      json.put("last", JSONObject.NULL);
      json.put("average_frames_per_second", JSONObject.NULL);
      json.put("maximum_gap", JSONObject.NULL);
      return json;
    }
    long first = timestamps.get(0);
    long last = timestamps.get(timestamps.size() - 1);
    json.put("first", Long.toString(first));
    json.put("last", Long.toString(last));
    json.put(
        "average_frames_per_second",
        timestamps.size() < 2 || last <= first
            ? JSONObject.NULL
            : (timestamps.size() - 1) * unitsPerSecond / (double) (last - first));
    if (timestamps.size() < 2) {
      json.put("maximum_gap", JSONObject.NULL);
      json.put("maximum_gap_previous", JSONObject.NULL);
      json.put("maximum_gap_current", JSONObject.NULL);
    } else {
      long maximum = Long.MIN_VALUE;
      long previousAtMaximum = 0;
      long currentAtMaximum = 0;
      for (int index = 1; index < timestamps.size(); ++index) {
        long gap = timestamps.get(index) - timestamps.get(index - 1);
        if (gap > maximum) {
          maximum = gap;
          previousAtMaximum = timestamps.get(index - 1);
          currentAtMaximum = timestamps.get(index);
        }
      }
      json.put("maximum_gap", Long.toString(maximum));
      json.put("maximum_gap_previous", Long.toString(previousAtMaximum));
      json.put("maximum_gap_current", Long.toString(currentAtMaximum));
    }
    return json;
  }

  private static String describe(Throwable failure) {
    StringBuilder description = new StringBuilder(failure.toString());
    for (Throwable cause = failure.getCause(); cause != null; cause = cause.getCause()) {
      description.append(" caused by ").append(cause);
    }
    return description.toString();
  }

  private record CameraSelection(String cameraId, int timestampSource, Size standbySize) {}

  private static final class WarmCamera {
    private final CameraManager manager;
    private final String cameraId;
    private final Size standbySize;
    private final HighSpeedProbe.Request request;
    private final HandlerThread handlerThread = new HandlerThread("warm-camera-transition");
    private final AtomicReference<Throwable> failure = new AtomicReference<>();
    private final AtomicBoolean transitioning = new AtomicBoolean();
    private final AtomicInteger openCount = new AtomicInteger();
    private final CountDownLatch deviceOpened = new CountDownLatch(1);
    private final CountDownLatch standbyConfigured = new CountDownLatch(1);
    private final CountDownLatch standbyFrames = new CountDownLatch(STANDBY_FRAMES);
    private final CountDownLatch standbyClosed = new CountDownLatch(1);
    private final CountDownLatch highSpeedConfigured = new CountDownLatch(1);
    private final CountDownLatch firstHighSpeedFrame = new CountDownLatch(1);
    private final List<Long> standbySensorTimestamps = new ArrayList<>();
    private final List<Long> highSpeedSensorTimestamps = new ArrayList<>();
    private final AtomicLong transitionRequestedNs = new AtomicLong();
    private final AtomicLong encoderStartedNs = new AtomicLong();
    private final AtomicLong standbyClosedNs = new AtomicLong();
    private final AtomicLong highSpeedConfiguredNs = new AtomicLong();
    private final AtomicLong firstHighSpeedFrameNs = new AtomicLong();
    private Handler handler;
    private ImageReader imageReader;
    private CameraDevice device;
    private CameraCaptureSession standbySession;
    private CameraConstrainedHighSpeedCaptureSession highSpeedSession;

    private WarmCamera(
        Context context,
        String cameraId,
        Size standbySize,
        HighSpeedProbe.Request request) {
      manager = context.getSystemService(CameraManager.class);
      this.cameraId = cameraId;
      this.standbySize = standbySize;
      this.request = request;
    }

    private void startStandby() throws Exception {
      handlerThread.start();
      handler = new Handler(handlerThread.getLooper());
      imageReader =
          ImageReader.newInstance(
              standbySize.getWidth(), standbySize.getHeight(), ImageFormat.YUV_420_888, 2);
      imageReader.setOnImageAvailableListener(
          reader -> {
            try (Image image = reader.acquireLatestImage()) {
              // Closing every image immediately keeps the low-rate session from back-pressuring.
            }
          },
          handler);
      manager.openCamera(
          cameraId,
          new CameraDevice.StateCallback() {
            @Override
            public void onOpened(CameraDevice opened) {
              openCount.incrementAndGet();
              device = opened;
              deviceOpened.countDown();
              createStandbySession(opened);
            }

            @Override
            public void onDisconnected(CameraDevice disconnected) {
              fail(new IllegalStateException("Camera disconnected during warm transition"));
              disconnected.close();
            }

            @Override
            public void onError(CameraDevice errored, int errorCode) {
              fail(new IllegalStateException("Camera error " + errorCode));
              errored.close();
            }
          },
          handler);
      await(deviceOpened, "camera device open");
      await(standbyConfigured, "standby session configuration");
      await(standbyFrames, "low-rate standby frames");
      throwIfFailed();
    }

    @SuppressWarnings("deprecation")
    private void createStandbySession(CameraDevice opened) {
      try {
        Surface standbySurface = imageReader.getSurface();
        opened.createCaptureSession(
            Collections.singletonList(standbySurface),
            new CameraCaptureSession.StateCallback() {
              @Override
              public void onConfigured(CameraCaptureSession configured) {
                standbySession = configured;
                standbyConfigured.countDown();
                try {
                  CaptureRequest.Builder builder =
                      opened.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                  builder.addTarget(standbySurface);
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
                fail(new IllegalStateException("Low-rate standby session configuration failed"));
              }

              @Override
              public void onClosed(CameraCaptureSession ignored) {
                standbyClosedNs.compareAndSet(0, SystemClock.elapsedRealtimeNanos());
                standbyClosed.countDown();
              }
            },
            handler);
      } catch (Throwable configureFailure) {
        fail(configureFailure);
      }
    }

    private void submitStandbyCapture(CameraCaptureSession session, CaptureRequest requestValue) {
      if (transitioning.get() || failure.get() != null) {
        return;
      }
      try {
        long requestSubmittedNs = SystemClock.elapsedRealtimeNanos();
        session.capture(
            requestValue,
            new CameraCaptureSession.CaptureCallback() {
              @Override
              public void onCaptureCompleted(
                  CameraCaptureSession ignored,
                  CaptureRequest ignoredRequest,
                  TotalCaptureResult result) {
                Long timestamp = result.get(CaptureResult.SENSOR_TIMESTAMP);
                if (timestamp == null) {
                  fail(new IllegalStateException("Standby frame has no sensor timestamp"));
                  return;
                }
                synchronized (standbySensorTimestamps) {
                  standbySensorTimestamps.add(timestamp);
                }
                standbyFrames.countDown();
                if (!transitioning.get()) {
                  long delayMillis =
                      LowRateCaptureSchedule.nextDelayMillis(
                          requestSubmittedNs,
                          SystemClock.elapsedRealtimeNanos(),
                          STANDBY_FRAME_INTERVAL_MILLIS);
                  handler.postDelayed(
                      () -> submitStandbyCapture(session, requestValue), delayMillis);
                }
              }

              @Override
              public void onCaptureFailed(
                  CameraCaptureSession ignored,
                  CaptureRequest ignoredRequest,
                  android.hardware.camera2.CaptureFailure captureFailure) {
                fail(
                    new IllegalStateException(
                        "Standby capture failed: " + captureFailure.getReason()));
              }
            },
            handler);
      } catch (Throwable captureFailure) {
        fail(captureFailure);
      }
    }

    private void transitionToHighSpeed(PreparedEncoder encoder) throws Exception {
      transitionRequestedNs.set(SystemClock.elapsedRealtimeNanos());
      transitioning.set(true);
      encoder.start();
      encoderStartedNs.set(SystemClock.elapsedRealtimeNanos());
      CameraCaptureSession currentStandby = standbySession;
      if (currentStandby == null) {
        throw new IllegalStateException("Standby session disappeared before transition");
      }
      try {
        currentStandby.abortCaptures();
      } finally {
        currentStandby.close();
      }
      await(standbyClosed, "standby session close");
      imageReader.close();
      imageReader = null;
      createHighSpeedSession(encoder.inputSurface());
      await(highSpeedConfigured, "high-speed session configuration");
      await(firstHighSpeedFrame, "first high-speed camera frame");
      encoder.awaitFirstOutput();
      throwIfFailed();
    }

    @SuppressWarnings("deprecation")
    private void createHighSpeedSession(Surface encoderSurface) {
      try {
        CaptureRequest.Builder builder =
            device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
        builder.addTarget(encoderSurface);
        builder.set(
            CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE,
            new Range<>(request.framesPerSecond(), request.framesPerSecond()));
        builder.set(
            CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
        device.createConstrainedHighSpeedCaptureSession(
            Collections.singletonList(encoderSurface),
            new CameraCaptureSession.StateCallback() {
              @Override
              public void onConfigured(CameraCaptureSession configured) {
                highSpeedSession = (CameraConstrainedHighSpeedCaptureSession) configured;
                highSpeedConfiguredNs.set(SystemClock.elapsedRealtimeNanos());
                highSpeedConfigured.countDown();
                try {
                  List<CaptureRequest> burst =
                      highSpeedSession.createHighSpeedRequestList(builder.build());
                  highSpeedSession.setRepeatingBurst(
                      burst,
                      new CameraCaptureSession.CaptureCallback() {
                        @Override
                        public void onCaptureStarted(
                            CameraCaptureSession ignored,
                            CaptureRequest ignoredRequest,
                            long ignoredTimestamp,
                            long ignoredFrameNumber) {
                          firstHighSpeedFrameNs.compareAndSet(
                              0, SystemClock.elapsedRealtimeNanos());
                          firstHighSpeedFrame.countDown();
                        }

                        @Override
                        public void onCaptureCompleted(
                            CameraCaptureSession ignored,
                            CaptureRequest ignoredRequest,
                            TotalCaptureResult result) {
                          Long timestamp = result.get(CaptureResult.SENSOR_TIMESTAMP);
                          if (timestamp == null) {
                            fail(
                                new IllegalStateException(
                                    "High-speed frame has no sensor timestamp"));
                            return;
                          }
                          synchronized (highSpeedSensorTimestamps) {
                            highSpeedSensorTimestamps.add(timestamp);
                          }
                        }

                        @Override
                        public void onCaptureFailed(
                            CameraCaptureSession ignored,
                            CaptureRequest ignoredRequest,
                            android.hardware.camera2.CaptureFailure captureFailure) {
                          fail(
                              new IllegalStateException(
                                  "High-speed capture failed: " + captureFailure.getReason()));
                        }
                      },
                      handler);
                } catch (Throwable startFailure) {
                  fail(startFailure);
                }
              }

              @Override
              public void onConfigureFailed(CameraCaptureSession ignored) {
                fail(new IllegalStateException("High-speed session configuration failed"));
              }
            },
            handler);
      } catch (Throwable configureFailure) {
        fail(configureFailure);
      }
    }

    private WarmCaptureTransitionTiming transitionTiming(PreparedEncoder encoder) {
      return new WarmCaptureTransitionTiming(
          transitionRequestedNs.get(),
          encoderStartedNs.get(),
          standbyClosedNs.get(),
          highSpeedConfiguredNs.get(),
          firstHighSpeedFrameNs.get(),
          encoder.firstOutputElapsedRealtimeNs());
    }

    private JSONObject standbyJson() throws Exception {
      return new JSONObject()
          .put("width", standbySize.getWidth())
          .put("height", standbySize.getHeight())
          .put("requested_interval_ms", STANDBY_FRAME_INTERVAL_MILLIS)
          .put("sensor_timestamps", timingJson(standbySensorTimestamps(), 1_000_000_000.0));
    }

    private JSONObject highSpeedJson(PreparedEncoder encoder) throws Exception {
      JSONObject json =
          new JSONObject()
              .put(
                  "sensor_timestamps",
                  timingJson(highSpeedSensorTimestamps(), 1_000_000_000.0))
              .put(
                  "acceptance_sensor_timestamps",
                  timingJson(
                      stableTail(
                          highSpeedSensorTimestamps(), STARTUP_CADENCE_DISCARD_NS),
                      1_000_000_000.0))
              .put("camera_open_count", cameraOpenCount());
      try {
        WarmCaptureTransitionTiming timing = transitionTiming(encoder);
        json.put("transition_timing", transitionTimingJson(timing));
      } catch (RuntimeException ignored) {
        json.put("transition_timing", JSONObject.NULL);
      }
      Throwable cameraFailure = failure();
      if (cameraFailure != null) {
        json.put("error", describe(cameraFailure));
      }
      return json;
    }

    private static JSONObject transitionTimingJson(WarmCaptureTransitionTiming timing)
        throws Exception {
      return new JSONObject()
          .put("transition_requested_elapsed_realtime_ns", Long.toString(timing.transitionRequestedNs()))
          .put("encoder_started_elapsed_realtime_ns", Long.toString(timing.encoderStartedNs()))
          .put("standby_session_closed_elapsed_realtime_ns", Long.toString(timing.standbySessionClosedNs()))
          .put("high_speed_session_configured_elapsed_realtime_ns", Long.toString(timing.highSpeedSessionConfiguredNs()))
          .put("first_high_speed_camera_frame_elapsed_realtime_ns", Long.toString(timing.firstHighSpeedCameraFrameNs()))
          .put("first_usable_encoded_frame_elapsed_realtime_ns", Long.toString(timing.firstUsableEncodedFrameNs()))
          .put("transition_to_encoder_start_ns", Long.toString(timing.transitionToEncoderStartNs()))
          .put("transition_to_standby_session_closed_ns", Long.toString(timing.transitionToStandbySessionClosedNs()))
          .put("transition_to_high_speed_session_configured_ns", Long.toString(timing.transitionToHighSpeedSessionConfiguredNs()))
          .put("transition_to_first_high_speed_camera_frame_ns", Long.toString(timing.transitionToFirstHighSpeedCameraFrameNs()))
          .put("transition_to_first_usable_encoded_frame_ns", Long.toString(timing.transitionToFirstUsableEncodedFrameNs()));
    }

    private void await(CountDownLatch latch, String operation) throws Exception {
      if (!latch.await(START_TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
        throwIfFailed();
        throw new IllegalStateException("Timed out waiting for " + operation);
      }
      throwIfFailed();
    }

    private void fail(Throwable cameraFailure) {
      failure.compareAndSet(null, cameraFailure);
      deviceOpened.countDown();
      standbyConfigured.countDown();
      while (standbyFrames.getCount() > 0) {
        standbyFrames.countDown();
      }
      standbyClosed.countDown();
      highSpeedConfigured.countDown();
      firstHighSpeedFrame.countDown();
    }

    private void throwIfFailed() {
      Throwable current = failure.get();
      if (current != null) {
        throw new IllegalStateException("Warm camera transition failed", current);
      }
    }

    private Throwable failure() {
      return failure.get();
    }

    private void recordFailure(Throwable stopFailure) {
      failure.compareAndSet(null, stopFailure);
    }

    private int cameraOpenCount() {
      return openCount.get();
    }

    private List<Long> standbySensorTimestamps() {
      synchronized (standbySensorTimestamps) {
        return new ArrayList<>(standbySensorTimestamps);
      }
    }

    private List<Long> highSpeedSensorTimestamps() {
      synchronized (highSpeedSensorTimestamps) {
        return new ArrayList<>(highSpeedSensorTimestamps);
      }
    }

    private void stop() throws Exception {
      transitioning.set(true);
      if (highSpeedSession != null) {
        try {
          highSpeedSession.stopRepeating();
          highSpeedSession.abortCaptures();
        } finally {
          highSpeedSession.close();
        }
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
      handlerThread.quitSafely();
      handlerThread.join(TimeUnit.SECONDS.toMillis(STOP_TIMEOUT_SECONDS));
      if (handlerThread.isAlive()) {
        throw new IllegalStateException("Warm camera handler thread did not stop");
      }
    }
  }

  private static final class PreparedEncoder {
    private final HighSpeedProbe.Request request;
    private final AtomicReference<Throwable> failure = new AtomicReference<>();
    private final AtomicBoolean stopRequested = new AtomicBoolean();
    private final AtomicLong firstOutputElapsedRealtimeNs = new AtomicLong();
    private final CountDownLatch firstOutput = new CountDownLatch(1);
    private final List<Long> presentationTimestampsUs = new ArrayList<>();
    private MediaCodec codec;
    private Surface inputSurface;
    private Thread drainThread;
    private String codecName;
    private long preparedElapsedRealtimeNs;

    private PreparedEncoder(HighSpeedProbe.Request request) {
      this.request = request;
    }

    private void prepare() throws Exception {
      codecName = selectEncoder(request);
      codec = MediaCodec.createByCodecName(codecName);
      MediaFormat format =
          MediaFormat.createVideoFormat(request.mime(), request.width(), request.height());
      format.setInteger(
          MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
      format.setInteger(MediaFormat.KEY_BIT_RATE, request.bitrateBitsPerSecond());
      format.setInteger(MediaFormat.KEY_FRAME_RATE, request.framesPerSecond());
      format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
      format.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0);
      format.setInteger(MediaFormat.KEY_PRIORITY, 0);
      format.setFloat(MediaFormat.KEY_OPERATING_RATE, request.framesPerSecond());
      codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
      inputSurface = codec.createInputSurface();
      preparedElapsedRealtimeNs = SystemClock.elapsedRealtimeNanos();
    }

    private void start() {
      codec.start();
      drainThread = new Thread(this::drain, "warm-transition-encoder");
      drainThread.start();
    }

    private Surface inputSurface() {
      return inputSurface;
    }

    private void drain() {
      MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
      try {
        while (!stopRequested.get()) {
          int index = codec.dequeueOutputBuffer(info, 20_000);
          if (index >= 0) {
            try {
              if (info.size > 0 && (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
                ByteBuffer output = codec.getOutputBuffer(index);
                if (output == null) {
                  throw new IllegalStateException("Encoder returned a null output buffer");
                }
                synchronized (presentationTimestampsUs) {
                  presentationTimestampsUs.add(info.presentationTimeUs);
                }
                firstOutputElapsedRealtimeNs.compareAndSet(0, SystemClock.elapsedRealtimeNanos());
                firstOutput.countDown();
              }
            } finally {
              codec.releaseOutputBuffer(index, false);
            }
          }
        }
      } catch (Throwable drainFailure) {
        failure.compareAndSet(null, drainFailure);
        firstOutput.countDown();
      }
    }

    private void awaitFirstOutput() throws Exception {
      if (!firstOutput.await(START_TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
        throw new IllegalStateException("Timed out waiting for the first encoded frame");
      }
      Throwable current = failure.get();
      if (current != null) {
        throw new IllegalStateException("Encoder failed during warm transition", current);
      }
    }

    private long firstOutputElapsedRealtimeNs() {
      return firstOutputElapsedRealtimeNs.get();
    }

    private List<Long> presentationTimestampsUs() {
      synchronized (presentationTimestampsUs) {
        return new ArrayList<>(presentationTimestampsUs);
      }
    }

    private Throwable failure() {
      return failure.get();
    }

    private void recordFailure(Throwable stopFailure) {
      failure.compareAndSet(null, stopFailure);
    }

    private void stop() throws Exception {
      stopRequested.set(true);
      if (drainThread != null) {
        drainThread.join(TimeUnit.SECONDS.toMillis(STOP_TIMEOUT_SECONDS));
        if (drainThread.isAlive()) {
          throw new IllegalStateException("Warm transition encoder drain thread did not stop");
        }
      }
      if (codec != null) {
        codec.stop();
        codec.release();
      }
      if (inputSurface != null) {
        inputSurface.release();
      }
    }

    private JSONObject toJson() throws Exception {
      JSONObject json =
          new JSONObject()
              .put("codec", codecName == null ? JSONObject.NULL : codecName)
              .put("prepared_elapsed_realtime_ns", Long.toString(preparedElapsedRealtimeNs))
              .put(
                  "first_output_elapsed_realtime_ns",
                  Long.toString(firstOutputElapsedRealtimeNs()))
              .put(
                  "presentation_timestamps",
                  timingJson(presentationTimestampsUs(), 1_000_000.0))
              .put(
                  "acceptance_presentation_timestamps",
                  timingJson(
                      stableTail(
                          presentationTimestampsUs(),
                          TimeUnit.NANOSECONDS.toMicros(STARTUP_CADENCE_DISCARD_NS)),
                      1_000_000.0));
      Throwable encoderFailure = failure();
      if (encoderFailure != null) {
        json.put("error", describe(encoderFailure));
      }
      return json;
    }
  }
}
