package com.agoessling.swingcapture.pose.inference;

import android.content.Context;
import android.os.SystemClock;
import android.util.Log;
import com.agoessling.swingcapture.pose.PoseLandmarkFrame;
import java.io.File;
import java.io.IOException;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.Executor;

/** Runs an app-private MP4 through the exact production pose trigger pipeline. */
public final class PoseReplayHilRunner {
  private static final String TAG = "PoseReplayHil";
  @FunctionalInterface
  public interface ReportWriter {
    File write(String json) throws IOException;
  }

  public record PublishedReport(PoseReplayReport report, File file) {
    public PublishedReport {
      Objects.requireNonNull(report, "report");
      Objects.requireNonNull(file, "file");
    }
  }

  /** Blocking implementation. Call this on a supplied background executor via {@link #runAsync}. */
  public PoseReplayReport run(
      Context context,
      File appPrivateRoot,
      File clip,
      PoseReplayConfiguration configuration) {
    Objects.requireNonNull(context, "context");
    Objects.requireNonNull(appPrivateRoot, "appPrivateRoot");
    Objects.requireNonNull(clip, "clip");
    Objects.requireNonNull(configuration, "configuration");
    String sourceId = safeSourceId(clip);
    PoseInferenceMetrics metrics = new PoseInferenceMetrics();
    PoseReplayEvaluation evaluation = new PoseReplayEvaluation(configuration);
    Optional<PoseInferenceDelegate> actualDelegate = Optional.empty();

    try {
      requireAppPrivateRoot(context, appPrivateRoot);
      try (MediaCodecReplayFrameSource source =
              MediaCodecReplayFrameSource.open(appPrivateRoot, clip);
          MediaPipePoseLandmarker landmarker =
              MediaPipePoseLandmarker.open(context, configuration.delegatePolicy())) {
        try {
          actualDelegate = Optional.of(landmarker.actualDelegate());
          while (evaluation.trace().size() < configuration.maximumFrames()) {
            RgbFrame frame = source.nextFrame();
            if (frame == null) {
              break;
            }
            metrics.recordOffered();
            metrics.recordScheduled();
            long startedNs = SystemClock.elapsedRealtimeNanos();
            long durationNs;
            try {
              PoseLandmarkFrame landmarks = landmarker.infer(frame);
              durationNs = Math.max(0, SystemClock.elapsedRealtimeNanos() - startedNs);
              evaluation.accept(landmarks, startedNs, durationNs);
              metrics.recordCompleted(durationNs, true);
            } catch (RuntimeException exception) {
              durationNs = Math.max(0, SystemClock.elapsedRealtimeNanos() - startedNs);
              metrics.recordCompleted(durationNs, false);
              throw exception;
            }
          }
          if (evaluation.trace().size() == configuration.maximumFrames()
              && source.nextFrame() != null) {
            return PoseReplayReport.failed(
                source.sourceId(),
                configuration,
                actualDelegate,
                metrics.snapshot(),
                evaluation.trace(),
                evaluation.finalState(),
                PoseReplayReport.FailureCode.FRAME_LIMIT_EXCEEDED);
          }
          if (evaluation.trace().isEmpty()) {
            return PoseReplayReport.failed(
                source.sourceId(),
                configuration,
                actualDelegate,
                metrics.snapshot(),
                evaluation.trace(),
                evaluation.finalState(),
                PoseReplayReport.FailureCode.RUNTIME_FAILURE);
          }
          return PoseReplayReport.completed(
              source.sourceId(),
              configuration,
              landmarker.actualDelegate(),
              metrics.snapshot(),
              evaluation.trace(),
              evaluation.finalState());
        } finally {
          Log.i(TAG, "Replay decoder metrics: " + source.performanceSnapshot().toLogString());
        }
      }
    } catch (IOException | RuntimeException exception) {
      return PoseReplayReport.failed(
          sourceId,
          configuration,
          actualDelegate,
          metrics.snapshot(),
          evaluation.trace(),
          evaluation.finalState(),
          PoseReplayReport.FailureCode.RUNTIME_FAILURE);
    }
  }

  public CompletableFuture<PoseReplayReport> runAsync(
      Executor executor,
      Context context,
      File appPrivateRoot,
      File clip,
      PoseReplayConfiguration configuration) {
    Objects.requireNonNull(executor, "executor");
    return CompletableFuture.supplyAsync(
        () -> run(context, appPrivateRoot, clip, configuration), executor);
  }

  public CompletableFuture<PublishedReport> runAndPersistAsync(
      Executor executor,
      Context context,
      File appPrivateRoot,
      File clip,
      PoseReplayConfiguration configuration,
      ReportWriter writer) {
    Objects.requireNonNull(writer, "writer");
    return runAsync(executor, context, appPrivateRoot, clip, configuration)
        .thenApply(
            report -> {
              try {
                return new PublishedReport(report, writer.write(report.toJson()));
              } catch (IOException exception) {
                throw new CompletionException(exception);
              }
            });
  }

  private static void requireAppPrivateRoot(Context context, File root) throws IOException {
    File dataRoot = context.getApplicationContext().getDataDir().getCanonicalFile();
    File canonicalRoot = root.getCanonicalFile();
    for (File cursor = canonicalRoot; cursor != null; cursor = cursor.getParentFile()) {
      if (dataRoot.equals(cursor)) {
        return;
      }
    }
    throw new IOException("replay root is outside app-private storage");
  }

  private static String safeSourceId(File clip) {
    String name = clip.getName();
    if (name.isEmpty() || name.length() > PoseReplayReportValidator.MAXIMUM_SOURCE_ID_CHARACTERS) {
      return "invalid_source";
    }
    for (int index = 0; index < name.length(); index++) {
      char character = name.charAt(index);
      if (character < 0x20 || character > 0x7e || character == '/' || character == '\\') {
        return "invalid_source";
      }
    }
    return name;
  }
}
