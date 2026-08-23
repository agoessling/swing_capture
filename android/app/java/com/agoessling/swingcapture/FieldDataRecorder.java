package com.agoessling.swingcapture;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.TotalCaptureResult;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTimestamp;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.media.MediaMuxer;
import android.media.MediaRecorder;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Process;
import android.os.SystemClock;
import android.util.Range;
import android.util.Size;
import android.view.Surface;
import java.io.File;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.Objects;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

/**
 * One-shot 720p30 H.264 plus mono PCM16 field-data recorder.
 *
 * <p>The recorder has exclusive ownership of the rear camera and microphone between {@link
 * #start()} and {@link #stop()}. It is intentionally a bounded prototype: one recording is
 * published atomically into app-private storage and every recording stops within ten minutes.
 */
public final class FieldDataRecorder implements AutoCloseable {
  public static final long MAXIMUM_DURATION_MILLIS = TimeUnit.MINUTES.toMillis(10);
  public static final long DEFAULT_DURATION_MILLIS = MAXIMUM_DURATION_MILLIS;
  public static final String STORAGE_DIRECTORY_NAME = "field_recordings";
  public static final String VIDEO_FILE_NAME = FieldRecordingPublisher.VIDEO_FILE_NAME;
  public static final String AUDIO_FILE_NAME = FieldRecordingPublisher.AUDIO_FILE_NAME;
  public static final String MANIFEST_FILE_NAME = FieldRecordingPublisher.MANIFEST_FILE_NAME;

  private static final int VIDEO_BITRATE_BITS_PER_SECOND = 8_000_000;
  private static final int AUDIO_BYTES_PER_FRAME = 2;
  private static final int AUDIO_READ_FRAMES = FieldRecordingManifest.AUDIO_SAMPLE_RATE_HZ / 10;
  private static final long START_TIMEOUT_MILLIS = 8_000;
  private static final long THREAD_STOP_TIMEOUT_MILLIS = 5_000;
  private static final long AUDIO_TIMESTAMP_BASE_UNCERTAINTY_NANOS = 250_000L;

  /** Immutable start request. The shared ID is identical on both phones. */
  public record Config(
      String recordingId,
      String sharedRecordingId,
      String nodeId,
      String role,
      int orientationDegrees,
      long maximumDurationMillis) {
    public Config {
      requireIdentifier(recordingId, "recordingId");
      requireIdentifier(sharedRecordingId, "sharedRecordingId");
      requireIdentifier(nodeId, "nodeId");
      Objects.requireNonNull(role, "role");
      if (role.isBlank() || role.length() > 64) {
        throw new IllegalArgumentException("role must be nonblank and bounded");
      }
      if ((orientationDegrees != 0
              && orientationDegrees != 90
              && orientationDegrees != 180
              && orientationDegrees != 270)
          || maximumDurationMillis < 1_000
          || maximumDurationMillis > MAXIMUM_DURATION_MILLIS) {
        throw new IllegalArgumentException("field recording orientation or duration is invalid");
      }
    }

    public static Config tenMinutePortrait(
        String recordingId, String sharedRecordingId, String nodeId, String role) {
      return new Config(
          recordingId,
          sharedRecordingId,
          nodeId,
          role,
          90,
          DEFAULT_DURATION_MILLIS);
    }
  }

  public enum State {
    NEW,
    STARTING,
    RECORDING,
    STOPPING,
    COMPLETED,
    FAILED,
    CLOSED
  }

  public enum StopReason {
    EXPLICIT,
    MAXIMUM_DURATION;

    String wireName() {
      return name().toLowerCase(Locale.ROOT);
    }
  }

  /** Thread-safe, lightweight status suitable for an HTTP response. */
  public record Status(
      State state,
      String recordingId,
      String sharedRecordingId,
      String createdAtUtc,
      long startedElapsedRealtimeNs,
      long elapsedMillis,
      long videoBytes,
      long audioFrames,
      String error) {}

  /** Atomically published recording bundle. */
  public record Result(
      String recordingId,
      String sharedRecordingId,
      String createdAtUtc,
      long startedElapsedRealtimeNs,
      long stoppedElapsedRealtimeNs,
      StopReason stopReason,
      File directory,
      File video,
      File audio,
      File manifest,
      long videoBytes,
      long audioFrames) {
    public Result {
      Objects.requireNonNull(stopReason, "stopReason");
      Objects.requireNonNull(directory, "directory");
      Objects.requireNonNull(video, "video");
      Objects.requireNonNull(audio, "audio");
      Objects.requireNonNull(manifest, "manifest");
    }
  }

  /** Callbacks can hand status to another executor; no callback owns recorder resources. */
  public interface Listener {
    default void onStatusChanged(Status status) {}

    default void onCompleted(Result result) {}

    default void onFailure(Throwable failure) {}
  }

  @FunctionalInterface
  interface StartHook {
    void beforeHardwareStart() throws Exception;
  }

  private record CameraSelection(String cameraId, int timestampSource) {}

  private final Context context;
  private final Config config;
  private final Listener listener;
  private final String createdAtUtc = Instant.now().toString();
  private final AtomicReference<State> state = new AtomicReference<>(State.NEW);
  private final AtomicReference<Throwable> failure = new AtomicReference<>();
  private final AtomicBoolean stopRequested = new AtomicBoolean();
  private final AtomicBoolean encoderEndRequested = new AtomicBoolean();
  private final AtomicLong videoBytes = new AtomicLong();
  private final AtomicLong audioFrames = new AtomicLong();
  private final CountDownLatch audioStarted = new CountDownLatch(1);
  private final CountDownLatch firstCameraFrame = new CountDownLatch(1);
  private final CountDownLatch muxerStarted = new CountDownLatch(1);
  private final CountDownLatch encoderFinished = new CountDownLatch(1);
  private final ScheduledExecutorService lifecycle =
      Executors.newSingleThreadScheduledExecutor(
          runnable -> new Thread(runnable, "field-recording-lifecycle"));
  private final List<FieldRecordingManifest.CameraFrame> cameraFrames = new ArrayList<>();
  private final List<FieldRecordingManifest.EncodedSample> encodedSamples = new ArrayList<>();
  private final List<FieldRecordingManifest.AudioTimestampObservation> audioTimestamps =
      new ArrayList<>();

  private volatile long startedElapsedRealtimeNs;
  private volatile long stoppedElapsedRealtimeNs;
  private volatile FieldRecordingManifest.ClockAnchor clockAnchor;
  private volatile Result result;
  private volatile int cameraTimestampSource = -1;
  private volatile int audioSource = -1;
  private volatile String videoCodec = "";
  private volatile long firstVideoPtsUs = -1;
  private volatile long lastVideoPtsUs = -1;
  private ScheduledFuture<?> maximumDurationStop;
  private File storageRoot;
  private File stagingDirectory;
  private File publishedDirectory;
  private File videoTemporary;
  private File audioTemporary;
  private MediaCodec encoder;
  private Surface encoderInputSurface;
  private MediaMuxer muxer;
  private Thread encoderThread;
  private AudioRecord audioRecord;
  private StreamingPcm16WavFile wavWriter;
  private Thread audioThread;
  private HandlerThread cameraThread;
  private CameraDevice cameraDevice;
  private CameraCaptureSession cameraSession;

  public FieldDataRecorder(Context context, Config config, Listener listener) {
    this.context = Objects.requireNonNull(context, "context").getApplicationContext();
    this.config = Objects.requireNonNull(config, "config");
    this.listener = Objects.requireNonNull(listener, "listener");
  }

  /** Starts all hardware and returns only after audio, camera, and MP4 muxing are live. */
  public synchronized void start() throws Exception {
    start(() -> {});
  }

  synchronized void start(StartHook startHook) throws Exception {
    Objects.requireNonNull(startHook, "startHook");
    if (!state.compareAndSet(State.NEW, State.STARTING)) {
      throw new IllegalStateException("field recorder can only be started once");
    }
    notifyStatus();
    try {
      startHook.beforeHardwareStart();
      requirePermissions();
      prepareStaging();
      startEncoder();
      long monotonicBeforeNs = System.nanoTime();
      long boottimeNs = SystemClock.elapsedRealtimeNanos();
      long monotonicAfterNs = System.nanoTime();
      clockAnchor =
          new FieldRecordingManifest.ClockAnchor(
              monotonicBeforeNs, boottimeNs, monotonicAfterNs);
      startedElapsedRealtimeNs = boottimeNs;
      startAudio();
      startCamera();
      awaitReady(audioStarted, "audio");
      awaitReady(firstCameraFrame, "camera");
      awaitReady(muxerStarted, "MP4 muxer");
      throwIfFailed();
      state.set(State.RECORDING);
      long elapsedMillis =
          TimeUnit.NANOSECONDS.toMillis(
              Math.max(0, SystemClock.elapsedRealtimeNanos() - startedElapsedRealtimeNs));
      maximumDurationStop =
          lifecycle.schedule(
              () -> stopFromLifecycle(StopReason.MAXIMUM_DURATION),
              Math.max(0, config.maximumDurationMillis() - elapsedMillis),
              TimeUnit.MILLISECONDS);
      notifyStatus();
    } catch (Throwable startFailure) {
      recordFailure(startFailure);
      cleanupFailedRecording();
      state.set(State.FAILED);
      lifecycle.shutdown();
      notifyStatus();
      listener.onFailure(startFailure);
      if (startFailure instanceof Exception exception) {
        throw exception;
      }
      throw new IllegalStateException("Unable to start field recording", startFailure);
    }
  }

  /** Explicitly stops, durably publishes, and returns the complete recording. */
  public synchronized Result stop() throws Exception {
    return stopInternal(StopReason.EXPLICIT);
  }

  public Status status() {
    long start = startedElapsedRealtimeNs;
    long end = stoppedElapsedRealtimeNs;
    long now = end == 0 ? SystemClock.elapsedRealtimeNanos() : end;
    long elapsedMillis = start == 0 ? 0 : TimeUnit.NANOSECONDS.toMillis(Math.max(0, now - start));
    Throwable problem = failure.get();
    return new Status(
        state.get(),
        config.recordingId(),
        config.sharedRecordingId(),
        createdAtUtc,
        start,
        elapsedMillis,
        videoBytes.get(),
        audioFrames.get(),
        problem == null ? "" : boundedError(problem));
  }

  public Result result() {
    return result;
  }

  @Override
  public synchronized void close() throws Exception {
    State current = state.get();
    if (current == State.RECORDING || current == State.STARTING) {
      stopInternal(StopReason.EXPLICIT);
    }
    if (state.get() != State.CLOSED) {
      state.set(State.CLOSED);
      lifecycle.shutdownNow();
      notifyStatus();
    }
  }

  private Result stopInternal(StopReason reason) throws Exception {
    if (state.get() == State.COMPLETED || state.get() == State.CLOSED) {
      if (result == null) {
        throw new IllegalStateException("field recording has no published result");
      }
      return result;
    }
    if (state.get() == State.NEW) {
      throw new IllegalStateException("field recording has not started");
    }
    if (state.get() == State.FAILED) {
      throw failedStateException();
    }
    state.set(State.STOPPING);
    stopRequested.set(true);
    if (maximumDurationStop != null) {
      maximumDurationStop.cancel(false);
    }
    notifyStatus();
    Throwable stopFailure = stopHardware();
    stoppedElapsedRealtimeNs = SystemClock.elapsedRealtimeNanos();
    if (stopFailure != null) {
      recordFailure(stopFailure);
    }
    if (failure.get() != null) {
      cleanupStagingBestEffort();
      state.set(State.FAILED);
      lifecycle.shutdown();
      notifyStatus();
      listener.onFailure(failure.get());
      throw failedStateException();
    }
    try {
      result = publish(reason);
      state.set(State.COMPLETED);
      lifecycle.shutdown();
      notifyStatus();
      listener.onCompleted(result);
      return result;
    } catch (Throwable publishFailure) {
      recordFailure(publishFailure);
      cleanupStagingBestEffort();
      state.set(State.FAILED);
      lifecycle.shutdown();
      notifyStatus();
      listener.onFailure(publishFailure);
      if (publishFailure instanceof Exception exception) {
        throw exception;
      }
      throw new IllegalStateException("Unable to publish field recording", publishFailure);
    }
  }

  private void stopFromLifecycle(StopReason reason) {
    try {
      synchronized (this) {
        stopInternal(reason);
      }
    } catch (Throwable lifecycleFailure) {
      recordFailure(lifecycleFailure);
    }
  }

  private void prepareStaging() throws Exception {
    storageRoot = new File(context.getFilesDir(), STORAGE_DIRECTORY_NAME);
    if ((!storageRoot.isDirectory() && !storageRoot.mkdirs())
        || Files.isSymbolicLink(storageRoot.toPath())) {
      throw new IllegalStateException("Unable to prepare field recording storage");
    }
    stagingDirectory = new File(storageRoot, config.recordingId() + ".tmp");
    publishedDirectory = new File(storageRoot, config.recordingId());
    if (stagingDirectory.exists()
        || publishedDirectory.exists()
        || !stagingDirectory.mkdir()) {
      throw new IllegalStateException("Field recording ID already exists");
    }
    SessionStagingCleanup.markOwned(storageRoot, stagingDirectory);
    AndroidDirectorySync.synchronize(stagingDirectory);
    AndroidDirectorySync.synchronize(storageRoot);
    videoTemporary = new File(stagingDirectory, VIDEO_FILE_NAME + ".tmp");
    audioTemporary = new File(stagingDirectory, AUDIO_FILE_NAME + ".tmp");
  }

  private void startEncoder() throws Exception {
    String encoderName = selectEncoder();
    encoder = MediaCodec.createByCodecName(encoderName);
    MediaFormat format =
        MediaFormat.createVideoFormat(
            MediaFormat.MIMETYPE_VIDEO_AVC,
            FieldRecordingManifest.WIDTH,
            FieldRecordingManifest.HEIGHT);
    format.setInteger(
        MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
    format.setInteger(MediaFormat.KEY_BIT_RATE, VIDEO_BITRATE_BITS_PER_SECOND);
    format.setInteger(
        MediaFormat.KEY_FRAME_RATE, FieldRecordingManifest.NOMINAL_FRAMES_PER_SECOND);
    format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
    format.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0);
    format.setInteger(MediaFormat.KEY_PRIORITY, 0);
    encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
    encoderInputSurface = encoder.createInputSurface();
    muxer =
        new MediaMuxer(
            videoTemporary.getAbsolutePath(), MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
    muxer.setOrientationHint(config.orientationDegrees());
    encoder.start();
    encoderThread = new Thread(this::drainEncoder, "field-recording-video");
    encoderThread.start();
  }

  private void drainEncoder() {
    MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
    boolean muxerIsStarted = false;
    int track = -1;
    try {
      while (true) {
        int index = encoder.dequeueOutputBuffer(info, 20_000);
        if (index == MediaCodec.INFO_TRY_AGAIN_LATER) {
          continue;
        }
        if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
          if (muxerIsStarted) {
            throw new IllegalStateException("encoder output format changed twice");
          }
          MediaFormat outputFormat = encoder.getOutputFormat();
          videoCodec = AvcMediaFormat.describe(outputFormat).rfc6381Codec();
          track = muxer.addTrack(outputFormat);
          muxer.start();
          muxerIsStarted = true;
          muxerStarted.countDown();
          continue;
        }
        if (index < 0) {
          continue;
        }
        try {
          if (info.size > 0 && (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
            if (!muxerIsStarted) {
              throw new IllegalStateException("encoded sample preceded the output format");
            }
            if (encodedSamples.size() >= FieldRecordingManifest.MAXIMUM_ENCODED_SAMPLES) {
              throw new IllegalStateException("encoded timing metadata reached its hard bound");
            }
            ByteBuffer output = encoder.getOutputBuffer(index);
            if (output == null) {
              throw new IllegalStateException("encoder returned a null output buffer");
            }
            long sourcePtsUs = info.presentationTimeUs;
            if (firstVideoPtsUs < 0) {
              firstVideoPtsUs = sourcePtsUs;
            }
            long mediaTimeUs = sourcePtsUs - firstVideoPtsUs;
            if (mediaTimeUs < 0
                || (!encodedSamples.isEmpty()
                    && mediaTimeUs
                        <= encodedSamples.get(encodedSamples.size() - 1).mediaTimeUs())) {
              throw new IllegalStateException("encoder produced nonmonotonic timestamps");
            }
            output.position(info.offset);
            output.limit(info.offset + info.size);
            MediaCodec.BufferInfo normalized = new MediaCodec.BufferInfo();
            normalized.set(0, info.size, mediaTimeUs, info.flags);
            muxer.writeSampleData(track, output, normalized);
            encodedSamples.add(
                new FieldRecordingManifest.EncodedSample(
                    encodedSamples.size(), sourcePtsUs, mediaTimeUs, info.flags, info.size));
            lastVideoPtsUs = sourcePtsUs;
            videoBytes.addAndGet(info.size);
          }
          if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
            break;
          }
        } finally {
          encoder.releaseOutputBuffer(index, false);
        }
      }
    } catch (Throwable encoderFailure) {
      failFromWorker(encoderFailure);
    } finally {
      if (muxerIsStarted) {
        try {
          muxer.stop();
        } catch (Throwable muxerFailure) {
          failFromWorker(muxerFailure);
        }
      }
      try {
        muxer.release();
      } catch (Throwable muxerFailure) {
        failFromWorker(muxerFailure);
      }
      encoderFinished.countDown();
    }
  }

  private void startAudio() throws Exception {
    int minimumBuffer =
        AudioRecord.getMinBufferSize(
            FieldRecordingManifest.AUDIO_SAMPLE_RATE_HZ,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT);
    if (minimumBuffer <= 0) {
      throw new IllegalStateException("Invalid minimum AudioRecord buffer " + minimumBuffer);
    }
    int bufferBytes =
        Math.max(
            Math.multiplyExact(minimumBuffer, 4),
            Math.multiplyExact(AUDIO_READ_FRAMES, AUDIO_BYTES_PER_FRAME));
    AudioManager manager = context.getSystemService(AudioManager.class);
    String supportsUnprocessed =
        manager == null
            ? null
            : manager.getProperty(AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED);
    // Keep field evidence on the same device-specific source as production impact detection.
    audioSource =
        !"barbet".equals(Build.DEVICE) && Boolean.parseBoolean(supportsUnprocessed)
            ? MediaRecorder.AudioSource.UNPROCESSED
            : MediaRecorder.AudioSource.VOICE_RECOGNITION;
    audioRecord =
        new AudioRecord.Builder()
            .setAudioSource(audioSource)
            .setAudioFormat(
                new AudioFormat.Builder()
                    .setSampleRate(FieldRecordingManifest.AUDIO_SAMPLE_RATE_HZ)
                    .setChannelMask(AudioFormat.CHANNEL_IN_MONO)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .build())
            .setBufferSizeInBytes(bufferBytes)
            .build();
    if (audioRecord.getState() != AudioRecord.STATE_INITIALIZED) {
      audioRecord.release();
      throw new IllegalStateException("Field recording AudioRecord did not initialize");
    }
    wavWriter = new StreamingPcm16WavFile(audioTemporary);
    audioThread = new Thread(this::recordAudio, "field-recording-audio");
    audioThread.start();
  }

  private void recordAudio() {
    Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);
    short[] samples = new short[AUDIO_READ_FRAMES];
    AudioTimestamp timestamp = new AudioTimestamp();
    try {
      audioRecord.startRecording();
      if (audioRecord.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) {
        throw new IllegalStateException("Field recording AudioRecord is not recording");
      }
      audioStarted.countDown();
      while (!stopRequested.get()) {
        int count = audioRecord.read(samples, 0, samples.length, AudioRecord.READ_BLOCKING);
        if (count <= 0 && stopRequested.get()) {
          break;
        }
        if (count < 0) {
          throw new IllegalStateException("AudioRecord read failed with " + count);
        }
        if (count == 0) {
          continue;
        }
        wavWriter.append(samples, 0, count);
        long wavEnd = audioFrames.addAndGet(count);
        long before = SystemClock.elapsedRealtimeNanos();
        int timestampResult = audioRecord.getTimestamp(timestamp, AudioTimestamp.TIMEBASE_BOOTTIME);
        long after = SystemClock.elapsedRealtimeNanos();
        if (timestampResult == AudioRecord.SUCCESS) {
          long uncertainty =
              AUDIO_TIMESTAMP_BASE_UNCERTAINTY_NANOS + Math.max(0, after - before) / 2;
          FieldRecordingManifest.AudioTimestampObservation observation =
              new FieldRecordingManifest.AudioTimestampObservation(
                  wavEnd, timestamp.framePosition, timestamp.nanoTime, uncertainty);
          if (audioTimestamps.isEmpty()
              || (observation.audioRecordFramePosition()
                      > audioTimestamps.get(audioTimestamps.size() - 1).audioRecordFramePosition()
                  && observation.boottimeNs()
                      > audioTimestamps.get(audioTimestamps.size() - 1).boottimeNs())) {
            if (audioTimestamps.size() >= FieldRecordingManifest.MAXIMUM_AUDIO_TIMESTAMPS) {
              throw new IllegalStateException("audio timing metadata reached its hard bound");
            }
            audioTimestamps.add(observation);
          }
        }
      }
    } catch (Throwable audioFailure) {
      if (!stopRequested.get()) {
        failFromWorker(audioFailure);
      }
    } finally {
      audioStarted.countDown();
    }
  }

  private void startCamera() throws Exception {
    CameraManager manager = context.getSystemService(CameraManager.class);
    CameraSelection selection = selectCamera(manager);
    cameraTimestampSource = selection.timestampSource();
    if (cameraTimestampSource != CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME) {
      throw new IllegalStateException("Rear camera timestamps are not in CLOCK_BOOTTIME");
    }
    cameraThread = new HandlerThread("field-recording-camera");
    cameraThread.start();
    Handler handler = new Handler(cameraThread.getLooper());
    manager.openCamera(
        selection.cameraId(),
        new CameraDevice.StateCallback() {
          @Override
          public void onOpened(CameraDevice opened) {
            cameraDevice = opened;
            createCameraSession(opened, handler);
          }

          @Override
          public void onDisconnected(CameraDevice disconnected) {
            disconnected.close();
            failFromWorker(new IllegalStateException("Field recording camera disconnected"));
          }

          @Override
          public void onError(CameraDevice errorDevice, int errorCode) {
            errorDevice.close();
            failFromWorker(new IllegalStateException("Field recording camera error " + errorCode));
          }
        },
        handler);
  }

  @SuppressWarnings("deprecation")
  private void createCameraSession(CameraDevice device, Handler handler) {
    try {
      CaptureRequest.Builder builder = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
      builder.addTarget(encoderInputSurface);
      builder.set(
          CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE,
          new Range<>(
              FieldRecordingManifest.NOMINAL_FRAMES_PER_SECOND,
              FieldRecordingManifest.NOMINAL_FRAMES_PER_SECOND));
      builder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
      device.createCaptureSession(
          Collections.singletonList(encoderInputSurface),
          new CameraCaptureSession.StateCallback() {
            @Override
            public void onConfigured(CameraCaptureSession configured) {
              cameraSession = configured;
              try {
                configured.setRepeatingRequest(
                    builder.build(),
                    new CameraCaptureSession.CaptureCallback() {
                      @Override
                      public void onCaptureStarted(
                          CameraCaptureSession ignoredSession,
                          CaptureRequest ignoredRequest,
                          long timestamp,
                          long ignoredFrameNumber) {
                        firstCameraFrame.countDown();
                      }

                      @Override
                      public void onCaptureCompleted(
                          CameraCaptureSession ignoredSession,
                          CaptureRequest ignoredRequest,
                          TotalCaptureResult captureResult) {
                        Long sensorTimestamp = captureResult.get(CaptureResult.SENSOR_TIMESTAMP);
                        if (sensorTimestamp == null) {
                          failFromWorker(
                              new IllegalStateException(
                                  "Camera2 result omitted SENSOR_TIMESTAMP"));
                          return;
                        }
                        if (cameraFrames.size() >= FieldRecordingManifest.MAXIMUM_CAMERA_FRAMES) {
                          failFromWorker(
                              new IllegalStateException(
                                  "camera timing metadata reached its hard bound"));
                          return;
                        }
                        try {
                          cameraFrames.add(
                              new FieldRecordingManifest.CameraFrame(
                                  captureResult.getFrameNumber(), sensorTimestamp));
                        } catch (Throwable timingFailure) {
                          failFromWorker(timingFailure);
                        }
                      }

                      @Override
                      public void onCaptureFailed(
                          CameraCaptureSession ignoredSession,
                          CaptureRequest ignoredRequest,
                          android.hardware.camera2.CaptureFailure captureFailure) {
                        int reason = captureFailure.getReason();
                        if (FieldRecordingStopPolicy.captureFailureIsFatal(
                            stopRequested.get(),
                            reason == android.hardware.camera2.CaptureFailure.REASON_FLUSHED)) {
                          failFromWorker(
                              new IllegalStateException(
                                  "Field recording capture failed: " + reason));
                        }
                      }
                    },
                    handler);
              } catch (Throwable configureFailure) {
                failFromWorker(configureFailure);
              }
            }

            @Override
            public void onConfigureFailed(CameraCaptureSession ignored) {
              failFromWorker(
                  new IllegalStateException("Field recording camera configuration failed"));
            }
          },
          handler);
    } catch (Throwable configureFailure) {
      failFromWorker(configureFailure);
    }
  }

  private CameraSelection selectCamera(CameraManager manager) throws Exception {
    Size requested = new Size(FieldRecordingManifest.WIDTH, FieldRecordingManifest.HEIGHT);
    for (String candidate : manager.getCameraIdList()) {
      CameraCharacteristics characteristics = manager.getCameraCharacteristics(candidate);
      Integer facing = characteristics.get(CameraCharacteristics.LENS_FACING);
      if (facing == null || facing != CameraCharacteristics.LENS_FACING_BACK) {
        continue;
      }
      StreamConfigurationMap streams =
          characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
      if (streams == null
          || streams.getOutputSizes(MediaCodec.class) == null
          || !Arrays.asList(streams.getOutputSizes(MediaCodec.class)).contains(requested)) {
        continue;
      }
      Range<Integer>[] ranges =
          characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
      if (ranges == null
          || !Arrays.asList(ranges)
              .contains(
                  new Range<>(
                      FieldRecordingManifest.NOMINAL_FRAMES_PER_SECOND,
                      FieldRecordingManifest.NOMINAL_FRAMES_PER_SECOND))) {
        continue;
      }
      Integer timestampSource =
          characteristics.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE);
      return new CameraSelection(candidate, timestampSource == null ? -1 : timestampSource);
    }
    throw new IllegalStateException("No rear camera supports 1280x720 at fixed 30 fps");
  }

  private String selectEncoder() {
    for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.ALL_CODECS).getCodecInfos()) {
      if (!info.isEncoder() || !info.isHardwareAccelerated()) {
        continue;
      }
      for (String type : info.getSupportedTypes()) {
        if (MediaFormat.MIMETYPE_VIDEO_AVC.equalsIgnoreCase(type)
            && info.getCapabilitiesForType(type)
                .getVideoCapabilities()
                .areSizeAndRateSupported(
                    FieldRecordingManifest.WIDTH,
                    FieldRecordingManifest.HEIGHT,
                    FieldRecordingManifest.NOMINAL_FRAMES_PER_SECOND)) {
          return info.getName();
        }
      }
    }
    throw new IllegalStateException("No hardware H.264 encoder supports 720p30");
  }

  private Throwable stopHardware() {
    Throwable aggregate = null;
    aggregate = stopCamera(aggregate);
    aggregate = stopAudio(aggregate);
    aggregate = stopEncoder(aggregate);
    return aggregate;
  }

  private Throwable stopCamera(Throwable aggregate) {
    if (cameraSession != null) {
      try {
        cameraSession.stopRepeating();
        cameraSession.abortCaptures();
      } catch (Throwable stopFailure) {
        aggregate = accumulate(aggregate, stopFailure);
      }
      try {
        cameraSession.close();
      } catch (Throwable closeFailure) {
        aggregate = accumulate(aggregate, closeFailure);
      }
    }
    if (cameraDevice != null) {
      try {
        cameraDevice.close();
      } catch (Throwable closeFailure) {
        aggregate = accumulate(aggregate, closeFailure);
      }
    }
    if (cameraThread != null) {
      cameraThread.quitSafely();
      aggregate = join(cameraThread, aggregate, "camera thread");
    }
    return aggregate;
  }

  private Throwable stopAudio(Throwable aggregate) {
    if (audioRecord != null) {
      try {
        if (audioRecord.getRecordingState() == AudioRecord.RECORDSTATE_RECORDING) {
          audioRecord.stop();
        }
      } catch (Throwable stopFailure) {
        aggregate = accumulate(aggregate, stopFailure);
      }
    }
    aggregate = join(audioThread, aggregate, "audio thread");
    if (audioRecord != null) {
      try {
        audioRecord.release();
      } catch (Throwable releaseFailure) {
        aggregate = accumulate(aggregate, releaseFailure);
      }
    }
    if (wavWriter != null) {
      try {
        wavWriter.close();
      } catch (Throwable closeFailure) {
        aggregate = accumulate(aggregate, closeFailure);
      }
    }
    return aggregate;
  }

  private Throwable stopEncoder(Throwable aggregate) {
    if (encoder != null && encoderEndRequested.compareAndSet(false, true)) {
      try {
        encoder.signalEndOfInputStream();
      } catch (Throwable signalFailure) {
        aggregate = accumulate(aggregate, signalFailure);
      }
    }
    if (encoderThread != null) {
      try {
        if (!encoderFinished.await(THREAD_STOP_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS)) {
          aggregate =
              accumulate(aggregate, new IllegalStateException("video encoder did not stop"));
        }
      } catch (InterruptedException interrupted) {
        Thread.currentThread().interrupt();
        aggregate = accumulate(aggregate, interrupted);
      }
    } else if (muxer != null) {
      // The encoder thread owns the muxer after it starts. A partial start owns it here.
      try {
        muxer.release();
      } catch (Throwable releaseFailure) {
        aggregate = accumulate(aggregate, releaseFailure);
      }
    }
    if (encoder != null) {
      try {
        encoder.stop();
      } catch (Throwable stopFailure) {
        aggregate = accumulate(aggregate, stopFailure);
      }
      try {
        encoder.release();
      } catch (Throwable releaseFailure) {
        aggregate = accumulate(aggregate, releaseFailure);
      }
    }
    if (encoderInputSurface != null) {
      encoderInputSurface.release();
    }
    return aggregate;
  }

  private Result publish(StopReason reason) throws Exception {
    if (encodedSamples.isEmpty() || cameraFrames.isEmpty() || audioTimestamps.isEmpty()) {
      throw new IllegalStateException("field recording lacks required timing evidence");
    }
    videoBytes.set(videoTemporary.length());
    long wavBytes = audioTemporary.length();
    FieldRecordingManifest.Data metadata =
        new FieldRecordingManifest.Data(
            config.recordingId(),
            config.sharedRecordingId(),
            config.nodeId(),
            config.role(),
            createdAtUtc,
            startedElapsedRealtimeNs,
            stoppedElapsedRealtimeNs,
            config.orientationDegrees(),
            reason.wireName(),
            videoCodec,
            videoTemporary.length(),
            firstVideoPtsUs,
            lastVideoPtsUs,
            audioSource,
            audioFrames.get(),
            wavBytes,
            cameraTimestampSource,
            clockAnchor,
            List.copyOf(cameraFrames),
            List.copyOf(encodedSamples),
            List.copyOf(audioTimestamps));
    FieldRecordingPublisher.PublishedFiles published =
        FieldRecordingPublisher.publish(
            storageRoot,
            stagingDirectory,
            publishedDirectory,
            videoTemporary,
            audioTemporary,
            metadata,
            FieldRecordingPublisher.fileSystemOperations(AndroidDirectorySync::synchronize));
    return new Result(
        config.recordingId(),
        config.sharedRecordingId(),
        createdAtUtc,
        startedElapsedRealtimeNs,
        stoppedElapsedRealtimeNs,
        reason,
        published.directory(),
        published.video(),
        published.audio(),
        published.manifest(),
        published.video().length(),
        audioFrames.get());
  }

  private void cleanupFailedRecording() {
    stopRequested.set(true);
    Throwable cleanupFailure = stopHardware();
    if (cleanupFailure != null) {
      recordFailure(cleanupFailure);
    }
    cleanupStagingBestEffort();
  }

  private void cleanupStagingBestEffort() {
    if (storageRoot != null && stagingDirectory != null && stagingDirectory.exists()) {
      if (!SessionStagingCleanup.cleanupFailedPublication(storageRoot, stagingDirectory)
          && !SessionStagingCleanup.cleanupAfterMarkFailure(storageRoot, stagingDirectory)) {
        recordFailure(new IllegalStateException("Unable to clean field recording staging"));
      }
    }
  }

  private void failFromWorker(Throwable workerFailure) {
    recordFailure(workerFailure);
    audioStarted.countDown();
    firstCameraFrame.countDown();
    muxerStarted.countDown();
    if (!stopRequested.get()) {
      try {
        lifecycle.execute(() -> stopFromLifecycle(StopReason.EXPLICIT));
      } catch (Throwable rejected) {
        recordFailure(rejected);
      }
    }
  }

  private void recordFailure(Throwable problem) {
    Objects.requireNonNull(problem, "problem");
    Throwable existing = failure.get();
    if (existing == null) {
      failure.compareAndSet(null, problem);
    } else if (existing != problem) {
      existing.addSuppressed(problem);
    }
  }

  private void awaitReady(CountDownLatch ready, String component) throws Exception {
    if (!ready.await(START_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS)) {
      throw new IllegalStateException("Timed out starting field recording " + component);
    }
    throwIfFailed();
  }

  private void throwIfFailed() {
    Throwable problem = failure.get();
    if (problem != null) {
      throw new IllegalStateException("Field recording hardware failed", problem);
    }
  }

  private IllegalStateException failedStateException() {
    return new IllegalStateException("Field recording failed", failure.get());
  }

  private void requirePermissions() {
    if (context.checkSelfPermission(Manifest.permission.CAMERA)
            != PackageManager.PERMISSION_GRANTED
        || context.checkSelfPermission(Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED) {
      throw new IllegalStateException("Camera and microphone permissions are required");
    }
  }

  private void notifyStatus() {
    listener.onStatusChanged(status());
  }

  private static void requireIdentifier(String value, String label) {
    Objects.requireNonNull(value, label);
    if (!value.matches("[A-Za-z0-9][A-Za-z0-9._-]{0,127}")) {
      throw new IllegalArgumentException(label + " contains unsupported characters");
    }
  }

  private static String boundedError(Throwable problem) {
    String message = problem.getClass().getSimpleName() + ": " + problem.getMessage();
    return message.length() <= 512 ? message : message.substring(0, 512);
  }

  private static Throwable accumulate(Throwable aggregate, Throwable addition) {
    if (aggregate == null) {
      return addition;
    }
    if (aggregate != addition) {
      aggregate.addSuppressed(addition);
    }
    return aggregate;
  }

  private static Throwable join(Thread thread, Throwable aggregate, String name) {
    if (thread == null || thread == Thread.currentThread()) {
      return aggregate;
    }
    try {
      thread.join(THREAD_STOP_TIMEOUT_MILLIS);
      if (thread.isAlive()) {
        return accumulate(aggregate, new IllegalStateException(name + " did not stop"));
      }
    } catch (InterruptedException interrupted) {
      Thread.currentThread().interrupt();
      return accumulate(aggregate, interrupted);
    }
    return aggregate;
  }

}
