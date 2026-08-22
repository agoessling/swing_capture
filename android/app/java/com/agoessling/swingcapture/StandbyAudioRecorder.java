package com.agoessling.swingcapture;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTimestamp;
import android.media.MediaRecorder;
import android.os.Build;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;
import com.agoessling.swingcapture.audio.AudioTimestampMapper;
import com.agoessling.swingcapture.audio.ImpactDetector;
import com.agoessling.swingcapture.diagnostics.PreviewEvidenceRing;
import com.agoessling.swingcapture.standby.StandbyDiagnosticCoordinator;
import java.util.Objects;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Exclusive owner of continuous 48 kHz mono PCM16 standby capture and diagnostic coordination.
 *
 * <p>This recorder is intended to run beside the 5 Hz pose standby path, not concurrently with a
 * high-speed capture engine that owns its own {@link AudioRecord}. The fixed 60-second audio ring
 * is allocated when {@link #start} is called. Preview evidence is copied only for a confirmed
 * impact or explicit operator tag, so normal audio blocks do no preview snapshot work.
 *
 * <p>Callbacks run serially on a dedicated thread. Audio metrics are coalesced, and the critical
 * callback queue is fixed-size: a blocked consumer therefore fails capture instead of growing
 * memory without bound. Callback implementations should still return promptly and hand frozen
 * evidence to their own bounded publisher.
 */
public final class StandbyAudioRecorder implements AutoCloseable {
  private static final String TAG = "StandbyAudio";
  private static final int BYTES_PER_FRAME = 2;
  private static final int READ_FRAMES = StandbyDiagnosticCoordinator.SAMPLE_RATE_HZ / 10;
  private static final int CALLBACK_QUEUE_CAPACITY = 16;
  private static final long START_TIMEOUT_MILLIS = 5_000;
  private static final long STOP_TIMEOUT_MILLIS = 5_000;
  private static final long AUDIO_TIMESTAMP_BASE_UNCERTAINTY_NANOS = 250_000L;

  /** Supplies the current detached pose-preview flight recorder at event registration. */
  @FunctionalInterface
  public interface PreviewSnapshotSource {
    PreviewEvidenceRing.Snapshot snapshot();
  }

  /**
   * Serialized callbacks. Implementations must not retain mutable PCM input or block this owner.
   */
  public interface Listener {
    default void onReady(int audioSource) {}

    default void onAudioMetrics(
        long endFramePosition, ImpactDetector.ProcessResult detectorMetrics) {}

    default void onDetectedImpact(StandbyDiagnosticCoordinator.EventMarker event) {}

    default void onOperatorTag(
        StandbyDiagnosticCoordinator.EventMarker event, boolean accepted) {}

    default void onEvidenceFrozen(StandbyDiagnosticCoordinator.FrozenEvidence evidence) {}

    default void onEventDropped(StandbyDiagnosticCoordinator.DroppedEvent event) {}

    default void onAudioDiscontinuity(
        StandbyDiagnosticCoordinator.StreamDiscontinuity discontinuity) {}

    default void onAudioTimestampRejected(
        AudioTimestampMapper.ObservationResult observation, long rejectionCount) {}

    default void onPreviewSnapshotFailure(Throwable failure) {}

    default void onFailure(Throwable failure) {}
  }

  private record AudioMetrics(
      long endFramePosition, ImpactDetector.ProcessResult detectorMetrics) {
    private AudioMetrics {
      Objects.requireNonNull(detectorMetrics, "detectorMetrics");
    }
  }

  private final AudioRecord audioRecord;
  private final int audioSource;
  private final PreviewSnapshotSource previewSnapshotSource;
  private final Listener listener;
  private final StandbyDiagnosticCoordinator coordinator;
  private final PreviewEvidenceRing.Snapshot emptyPreviewSnapshot;
  private final CallbackDispatcher callbacks;
  private final CountDownLatch started = new CountDownLatch(1);
  private final AtomicBoolean stopping = new AtomicBoolean();
  private final AtomicBoolean closed = new AtomicBoolean();
  private final AtomicReference<Throwable> failure = new AtomicReference<>();
  private final AtomicLong rejectedAudioTimestamps = new AtomicLong();
  private final Thread audioThread;

  /** Opens and starts the sole standby microphone owner, waiting until AudioRecord is recording. */
  public static StandbyAudioRecorder start(
      Context context, PreviewSnapshotSource previewSnapshotSource, Listener listener)
      throws InterruptedException {
    Objects.requireNonNull(context, "context");
    Objects.requireNonNull(previewSnapshotSource, "previewSnapshotSource");
    Objects.requireNonNull(listener, "listener");
    if (context.checkSelfPermission(Manifest.permission.RECORD_AUDIO)
        != PackageManager.PERMISSION_GRANTED) {
      throw new IllegalStateException("Microphone permission is required for standby diagnostics");
    }

    int minimumBufferBytes =
        AudioRecord.getMinBufferSize(
            StandbyDiagnosticCoordinator.SAMPLE_RATE_HZ,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT);
    if (minimumBufferBytes <= 0) {
      throw new IllegalStateException("Invalid minimum AudioRecord buffer " + minimumBufferBytes);
    }
    int bufferBytes =
        Math.max(
            Math.multiplyExact(minimumBufferBytes, 4),
            StandbyDiagnosticCoordinator.SAMPLE_RATE_HZ / 5 * BYTES_PER_FRAME);
    int source = selectAudioSource(context);
    AudioRecord record =
        new AudioRecord.Builder()
            .setAudioSource(source)
            .setAudioFormat(
                new AudioFormat.Builder()
                    .setSampleRate(StandbyDiagnosticCoordinator.SAMPLE_RATE_HZ)
                    .setChannelMask(AudioFormat.CHANNEL_IN_MONO)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .build())
            .setBufferSizeInBytes(bufferBytes)
            .build();
    if (record.getState() != AudioRecord.STATE_INITIALIZED) {
      record.release();
      throw new IllegalStateException("Standby AudioRecord did not initialize");
    }

    StandbyAudioRecorder owner =
        new StandbyAudioRecorder(
            record,
            source,
            previewSnapshotSource,
            listener,
            new StandbyDiagnosticCoordinator(
                StandbyDiagnosticCoordinator.Config.defaults(),
                DeviceAudioDetectorPolicy.forDevice(Build.MANUFACTURER, Build.MODEL),
                AudioTimestampMapper.Config.defaults()));
    owner.audioThread.start();
    if (!owner.started.await(START_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS)) {
      owner.close();
      throw new IllegalStateException("Timed out starting standby AudioRecord");
    }
    Throwable startFailure = owner.failure.get();
    if (startFailure != null) {
      owner.close();
      throw new IllegalStateException("Standby AudioRecord failed to start", startFailure);
    }
    return owner;
  }

  private StandbyAudioRecorder(
      AudioRecord audioRecord,
      int audioSource,
      PreviewSnapshotSource previewSnapshotSource,
      Listener listener,
      StandbyDiagnosticCoordinator coordinator) {
    this.audioRecord = Objects.requireNonNull(audioRecord, "audioRecord");
    this.audioSource = audioSource;
    this.previewSnapshotSource =
        Objects.requireNonNull(previewSnapshotSource, "previewSnapshotSource");
    this.listener = Objects.requireNonNull(listener, "listener");
    this.coordinator = Objects.requireNonNull(coordinator, "coordinator");
    emptyPreviewSnapshot = new PreviewEvidenceRing(1, 1, 1).snapshot();
    callbacks = new CallbackDispatcher(listener, this::failFromCallback);
    audioThread = new Thread(this::recordAudio, "standby-diagnostic-audio");
  }

  /**
   * Registers an operator button receipt against the latest PCM frame already read.
   *
   * <p>The result is immediate, while the corresponding frozen evidence callback follows after
   * the configured two-second post-roll. No impact timestamp is guessed.
   */
  public synchronized StandbyDiagnosticCoordinator.OperatorTagResult tagOperator() {
    requireRunning();
    PreviewEvidenceRing.Snapshot preview = snapshotPreviewBestEffort();
    StandbyDiagnosticCoordinator.OperatorTagResult result =
        coordinator.tagOperator(SystemClock.elapsedRealtimeNanos(), preview);
    if (!callbacks.dispatchCritical(() -> dispatchOperatorResult(result))) {
      fail(new IllegalStateException("Standby diagnostic callback queue overflow"));
    }
    return result;
  }

  public int audioSource() {
    return audioSource;
  }

  public int pendingEventCount() {
    return coordinator.pendingEventCount();
  }

  public long retainedAudioEndFramePosition() {
    return coordinator.retainedAudioEndFramePosition();
  }

  public Throwable failure() {
    return failure.get();
  }

  @Override
  public synchronized void close() {
    if (!closed.compareAndSet(false, true)) {
      return;
    }
    stopping.set(true);
    stopAudioRecord();
    join(audioThread, STOP_TIMEOUT_MILLIS);
    audioRecord.release();
    callbacks.close(STOP_TIMEOUT_MILLIS);
  }

  private void recordAudio() {
    Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);
    short[] samples = new short[READ_FRAMES];
    AudioTimestamp timestamp = new AudioTimestamp();
    long firstFramePosition = 0;
    try {
      audioRecord.startRecording();
      if (audioRecord.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) {
        throw new IllegalStateException("Standby AudioRecord is not recording");
      }
      started.countDown();
      if (!callbacks.dispatchCritical(() -> listener.onReady(audioSource))) {
        throw new IllegalStateException("Standby diagnostic callback queue overflow");
      }
      while (!stopping.get()) {
        int frameCount =
            audioRecord.read(samples, 0, samples.length, AudioRecord.READ_BLOCKING);
        if (frameCount <= 0 && stopping.get()) {
          return;
        }
        if (frameCount < 0) {
          throw new IllegalStateException("Standby AudioRecord read failed with " + frameCount);
        }
        if (frameCount == 0) {
          continue;
        }

        observeTimestamp(timestamp);
        StandbyDiagnosticCoordinator.AppendResult result =
            coordinator.appendPcm16(
                samples,
                0,
                frameCount,
                firstFramePosition,
                this::snapshotPreviewBestEffort);
        firstFramePosition = Math.addExact(firstFramePosition, frameCount);
        if (!dispatchAppendResult(firstFramePosition, result)) {
          throw new IllegalStateException("Standby diagnostic callback queue overflow");
        }
      }
    } catch (Throwable caught) {
      if (!stopping.get()) {
        fail(caught);
      }
    } finally {
      started.countDown();
    }
  }

  private void observeTimestamp(AudioTimestamp timestamp) {
    long before = SystemClock.elapsedRealtimeNanos();
    int timestampResult =
        audioRecord.getTimestamp(timestamp, AudioTimestamp.TIMEBASE_BOOTTIME);
    long after = SystemClock.elapsedRealtimeNanos();
    if (timestampResult != AudioRecord.SUCCESS) {
      return;
    }
    long uncertainty =
        Math.addExact(
            AUDIO_TIMESTAMP_BASE_UNCERTAINTY_NANOS,
            Math.max(0, after - before) / 2);
    AudioTimestampMapper.ObservationResult observation =
        coordinator.observeAudioTimestamp(
            timestamp.framePosition, timestamp.nanoTime, uncertainty);
    if (!observation.accepted()) {
      long count = rejectedAudioTimestamps.incrementAndGet();
      if (count == 1 || count % 100 == 0) {
        callbacks.dispatchCritical(() -> listener.onAudioTimestampRejected(observation, count));
      }
    }
  }

  private boolean dispatchAppendResult(
      long endFramePosition, StandbyDiagnosticCoordinator.AppendResult result) {
    boolean hasEffects =
        !result.registeredEvents().isEmpty()
            || !result.frozenEvidence().isEmpty()
            || !result.droppedEvents().isEmpty()
            || result.discontinuity().isPresent();
    boolean accepted =
        !hasEffects
            || callbacks.dispatchCritical(
                () -> {
                  for (StandbyDiagnosticCoordinator.EventMarker event :
                      result.registeredEvents()) {
                    listener.onDetectedImpact(event);
                  }
                  for (StandbyDiagnosticCoordinator.FrozenEvidence evidence :
                      result.frozenEvidence()) {
                    listener.onEvidenceFrozen(evidence);
                  }
                  for (StandbyDiagnosticCoordinator.DroppedEvent dropped :
                      result.droppedEvents()) {
                    listener.onEventDropped(dropped);
                  }
                  result.discontinuity().ifPresent(listener::onAudioDiscontinuity);
                });
    callbacks.dispatchMetrics(new AudioMetrics(endFramePosition, result.detectorMetrics()));
    return accepted;
  }

  private void dispatchOperatorResult(StandbyDiagnosticCoordinator.OperatorTagResult result) {
    listener.onOperatorTag(result.event(), result.accepted());
    for (StandbyDiagnosticCoordinator.FrozenEvidence evidence : result.frozenEvidence()) {
      listener.onEvidenceFrozen(evidence);
    }
    for (StandbyDiagnosticCoordinator.DroppedEvent dropped : result.droppedEvents()) {
      listener.onEventDropped(dropped);
    }
  }

  private PreviewEvidenceRing.Snapshot snapshotPreviewBestEffort() {
    try {
      return Objects.requireNonNull(previewSnapshotSource.snapshot(), "preview snapshot");
    } catch (RuntimeException snapshotFailure) {
      callbacks.dispatchCritical(() -> listener.onPreviewSnapshotFailure(snapshotFailure));
      return emptyPreviewSnapshot;
    }
  }

  private void requireRunning() {
    if (closed.get() || stopping.get()) {
      throw new IllegalStateException("Standby AudioRecord is not running");
    }
    Throwable currentFailure = failure.get();
    if (currentFailure != null) {
      throw new IllegalStateException("Standby AudioRecord failed", currentFailure);
    }
  }

  private void fail(Throwable caught) {
    Objects.requireNonNull(caught, "caught");
    if (failure.compareAndSet(null, caught)) {
      stopping.set(true);
      started.countDown();
      stopAudioRecord();
      callbacks.dispatchFailure(caught);
    }
  }

  private void failFromCallback(Throwable caught) {
    if (failure.compareAndSet(null, caught)) {
      stopping.set(true);
      started.countDown();
      stopAudioRecord();
      callbacks.dispatchFailure(caught);
    }
  }

  private void stopAudioRecord() {
    try {
      if (audioRecord.getRecordingState() == AudioRecord.RECORDSTATE_RECORDING) {
        audioRecord.stop();
      }
    } catch (IllegalStateException stopFailure) {
      Log.w(TAG, "Unable to stop standby AudioRecord", stopFailure);
    }
  }

  private static int selectAudioSource(Context context) {
    if ("barbet".equals(Build.DEVICE)) {
      return MediaRecorder.AudioSource.VOICE_RECOGNITION;
    }
    AudioManager audioManager = context.getSystemService(AudioManager.class);
    String supportsUnprocessed =
        audioManager == null
            ? null
            : audioManager.getProperty(AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED);
    return Boolean.parseBoolean(supportsUnprocessed)
        ? MediaRecorder.AudioSource.UNPROCESSED
        : MediaRecorder.AudioSource.VOICE_RECOGNITION;
  }

  private static void join(Thread thread, long timeoutMillis) {
    if (thread == Thread.currentThread()) {
      return;
    }
    try {
      thread.join(timeoutMillis);
      if (thread.isAlive()) {
        Log.e(TAG, "Standby audio thread did not stop within timeout");
      }
    } catch (InterruptedException interrupted) {
      Thread.currentThread().interrupt();
    }
  }

  /** One bounded serial callback worker with a coalesced metrics slot. */
  private static final class CallbackDispatcher {
    private final Listener listener;
    private final java.util.function.Consumer<Throwable> callbackFailure;
    private final ArrayBlockingQueue<Runnable> queue =
        new ArrayBlockingQueue<>(CALLBACK_QUEUE_CAPACITY);
    private final AtomicReference<AudioMetrics> latestMetrics = new AtomicReference<>();
    private final AtomicBoolean metricsScheduled = new AtomicBoolean();
    private final AtomicBoolean stopping = new AtomicBoolean();
    private final Thread thread;

    private CallbackDispatcher(
        Listener listener, java.util.function.Consumer<Throwable> callbackFailure) {
      this.listener = Objects.requireNonNull(listener, "listener");
      this.callbackFailure = Objects.requireNonNull(callbackFailure, "callbackFailure");
      thread = new Thread(this::run, "standby-diagnostic-callbacks");
      thread.start();
    }

    private boolean dispatchCritical(Runnable callback) {
      Objects.requireNonNull(callback, "callback");
      return !stopping.get() && queue.offer(callback);
    }

    private void dispatchMetrics(AudioMetrics metrics) {
      latestMetrics.set(Objects.requireNonNull(metrics, "metrics"));
      if (metricsScheduled.compareAndSet(false, true)
          && !queue.offer(this::deliverLatestMetrics)) {
        metricsScheduled.set(false);
      }
    }

    private void deliverLatestMetrics() {
      AudioMetrics metrics = latestMetrics.getAndSet(null);
      if (metrics != null) {
        listener.onAudioMetrics(metrics.endFramePosition(), metrics.detectorMetrics());
      }
      metricsScheduled.set(false);
      if (latestMetrics.get() != null
          && metricsScheduled.compareAndSet(false, true)
          && !queue.offer(this::deliverLatestMetrics)) {
        metricsScheduled.set(false);
      }
    }

    private void dispatchFailure(Throwable failure) {
      queue.clear();
      latestMetrics.set(null);
      metricsScheduled.set(false);
      queue.offer(() -> listener.onFailure(failure));
    }

    private void run() {
      while (!stopping.get() || !queue.isEmpty()) {
        try {
          Runnable callback = queue.poll(250, TimeUnit.MILLISECONDS);
          if (callback != null) {
            callback.run();
          }
        } catch (InterruptedException interrupted) {
          if (stopping.get()) {
            Thread.currentThread().interrupt();
            return;
          }
        } catch (Throwable failure) {
          callbackFailure.accept(failure);
        }
      }
    }

    private void close(long timeoutMillis) {
      stopping.set(true);
      join(thread, timeoutMillis);
      if (thread.isAlive()) {
        thread.interrupt();
        join(thread, Math.min(timeoutMillis, 1_000));
      }
    }
  }
}
