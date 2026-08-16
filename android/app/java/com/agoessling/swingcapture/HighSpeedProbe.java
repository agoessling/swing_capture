package com.agoessling.swingcapture;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraConstrainedHighSpeedCaptureSession;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CameraMetadata;
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
import android.os.Handler;
import android.os.HandlerThread;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.Range;
import android.util.Size;
import android.view.Surface;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.UUID;
import org.json.JSONArray;
import org.json.JSONObject;

/** Runs a short, non-retained high-speed camera and microphone acceptance probe. */
public final class HighSpeedProbe {
  private static final int AUDIO_SAMPLE_RATE_HZ = 48_000;
  private static final long START_TIMEOUT_SECONDS = 10;
  private static final long THREAD_STOP_TIMEOUT_SECONDS = 5;

  /** Parameters for one bounded probe. */
  public static final class Request {
    private final int width;
    private final int height;
    private final int framesPerSecond;
    private final int durationMillis;
    private final int bitrateBitsPerSecond;
    private final String mime;

    public Request(
        int width,
        int height,
        int framesPerSecond,
        int durationMillis,
        int bitrateBitsPerSecond,
        String mime) {
      if (width <= 0 || height <= 0 || framesPerSecond <= 0) {
        throw new IllegalArgumentException("Probe dimensions and frame rate must be positive");
      }
      if (durationMillis < 500 || durationMillis > 15_000) {
        throw new IllegalArgumentException("Probe duration must be between 500 and 15000 ms");
      }
      if (bitrateBitsPerSecond < 1_000_000 || bitrateBitsPerSecond > 100_000_000) {
        throw new IllegalArgumentException("Probe bitrate must be between 1 and 100 Mbit/s");
      }
      if (!MediaFormat.MIMETYPE_VIDEO_AVC.equals(mime)
          && !MediaFormat.MIMETYPE_VIDEO_HEVC.equals(mime)) {
        throw new IllegalArgumentException("Unsupported probe MIME type " + mime);
      }
      this.width = width;
      this.height = height;
      this.framesPerSecond = framesPerSecond;
      this.durationMillis = durationMillis;
      this.bitrateBitsPerSecond = bitrateBitsPerSecond;
      this.mime = mime;
    }

    public static Request pixel6AcceptanceGate() {
      return new Request(
          1920, 1080, 240, 3_000, 24_000_000, MediaFormat.MIMETYPE_VIDEO_AVC);
    }

    public int width() {
      return width;
    }

    public int height() {
      return height;
    }

    public int framesPerSecond() {
      return framesPerSecond;
    }

    public int durationMillis() {
      return durationMillis;
    }

    public int bitrateBitsPerSecond() {
      return bitrateBitsPerSecond;
    }

    public String mime() {
      return mime;
    }

    private JSONObject toJson() throws Exception {
      JSONObject json = new JSONObject();
      json.put("width", width);
      json.put("height", height);
      json.put("frames_per_second", framesPerSecond);
      json.put("duration_ms", durationMillis);
      json.put("bitrate_bits_per_second", bitrateBitsPerSecond);
      json.put("mime", mime);
      return json;
    }
  }

  private HighSpeedProbe() {}

  public static JSONObject run(
      Context context, CaptureConfigurationSnapshot captureConfiguration, Request request)
      throws Exception {
    return runInternal(context, captureConfiguration, request, false);
  }

  public static JSONObject runRetained(
      Context context, CaptureConfigurationSnapshot captureConfiguration, Request request)
      throws Exception {
    return runInternal(context, captureConfiguration, request, true);
  }

  private static JSONObject runInternal(
      Context context,
      CaptureConfigurationSnapshot captureConfiguration,
      Request request,
      boolean retainSession) throws Exception {
    captureConfiguration.requireAssignedRole();
    JSONObject report = new JSONObject();
    report.put("schema_version", 1);
    report.put("report_type", "android_high_speed_probe");
    report.put("created_at_utc", Instant.now().toString());
    report.put("node_id", captureConfiguration.nodeId());
    report.put("role", captureConfiguration.role().wireName());
    report.put("request", request.toJson());
    report.put("retain_session_requested", retainSession);
    report.put("thermal_status_before", thermalStatus(context));

    EncoderCapture encoder = null;
    AudioCapture audio = null;
    CameraCapture camera = null;
    CameraSelection selection = null;
    Throwable fatalFailure = null;
    long probeStartNanos = SystemClock.elapsedRealtimeNanos();
    long probeStopNanos = probeStartNanos;
    long audioFramesAtStart = 0;
    long audioFramesAtStop = 0;
    try {
      requirePermission(context, Manifest.permission.CAMERA);
      requirePermission(context, Manifest.permission.RECORD_AUDIO);
      selection = selectCamera(context, request);
      report.put("camera_id", selection.cameraId);
      report.put("camera_timestamp_source", selection.timestampSource);

      encoder = new EncoderCapture(request, retainSession);
      encoder.start();
      audio = new AudioCapture(context);
      audio.start();
      camera = new CameraCapture(context, selection.cameraId, request, encoder.inputSurface());
      camera.start();

      probeStartNanos = SystemClock.elapsedRealtimeNanos();
      audioFramesAtStart = audio.framesRead();
      Thread.sleep(request.durationMillis());
      probeStopNanos = SystemClock.elapsedRealtimeNanos();
      audioFramesAtStop = audio.framesRead();
    } catch (Throwable failure) {
      fatalFailure = failure;
      probeStopNanos = SystemClock.elapsedRealtimeNanos();
      if (audio != null) {
        audioFramesAtStop = audio.framesRead();
      }
    } finally {
      if (camera != null) {
        try {
          camera.stop();
        } catch (Throwable stopFailure) {
          camera.recordFailure(stopFailure);
        }
      }
      if (audio != null) {
        try {
          audio.stop();
        } catch (Throwable stopFailure) {
          audio.recordFailure(stopFailure);
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

    return finishReport(
        context,
        captureConfiguration,
        request,
        retainSession,
        report,
        encoder,
        audio,
        camera,
        selection,
        fatalFailure,
        probeStartNanos,
        probeStopNanos,
        audioFramesAtStart,
        audioFramesAtStop);
  }

  private static JSONObject finishReport(
      Context context,
      CaptureConfigurationSnapshot captureConfiguration,
      Request request,
      boolean retainSession,
      JSONObject report,
      EncoderCapture encoder,
      AudioCapture audio,
      CameraCapture camera,
      CameraSelection selection,
      Throwable fatalFailure,
      long probeStartNanos,
      long probeStopNanos,
      long audioFramesAtStart,
      long audioFramesAtStop) throws Exception {
    CameraEncoderTimestampAlignment.Result timestampAlignment =
        camera != null && encoder != null
            ? CameraEncoderTimestampAlignment.calculate(
                camera.timestampSamples(), encoder.presentationTimesUs())
            : CameraEncoderTimestampAlignment.calculate(
                Collections.emptyList(), Collections.emptyList());

    report.put("probe_start_elapsed_realtime_ns", probeStartNanos);
    report.put("probe_stop_elapsed_realtime_ns", probeStopNanos);
    report.put("probe_elapsed_ms", (probeStopNanos - probeStartNanos) / 1_000_000.0);
    report.put("thermal_status_after", thermalStatus(context));
    if (camera != null) {
      report.put("camera", camera.toJson());
    }
    if (encoder != null) {
      report.put(
          "encoder", encoder.toJson(probeStartNanos, probeStopNanos, timestampAlignment));
    }
    if (audio != null) {
      report.put(
          "audio",
          audio.toJson(
              probeStartNanos, probeStopNanos, audioFramesAtStart, audioFramesAtStop));
    }
    if (fatalFailure != null) {
      report.put("fatal_error", describe(fatalFailure));
    }

    JSONArray failures =
        acceptanceFailures(
            request,
            selection,
            camera,
            encoder,
            timestampAlignment,
            audio,
            fatalFailure,
            probeStartNanos,
            probeStopNanos,
            audioFramesAtStart,
            audioFramesAtStop);
    if (retainSession && failures.length() == 0 && encoder != null && selection != null) {
      try {
        long syntheticTriggerNanos = probeStartNanos + TimeUnit.MILLISECONDS.toNanos(1_900);
        JSONObject session =
            publishRetainedSession(
                context,
                captureConfiguration,
                request,
                selection,
                encoder,
                timestampAlignment,
                syntheticTriggerNanos);
        report.put("retained_session", session);
      } catch (Throwable publishFailure) {
        report.put("retained_session_error", describe(publishFailure));
        failures.put("retained_session_publish_failure");
      }
    }
    report.put("acceptance_failures", failures);
    report.put("passed", failures.length() == 0);
    return report;
  }

  private static void requirePermission(Context context, String permission) {
    if (context.checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) {
      throw new IllegalStateException("Required permission is not granted: " + permission);
    }
  }

  private static int thermalStatus(Context context) {
    return context.getSystemService(PowerManager.class).getCurrentThermalStatus();
  }

  private static CameraSelection selectCamera(Context context, Request request) throws Exception {
    CameraManager manager = context.getSystemService(CameraManager.class);
    for (String cameraId : manager.getCameraIdList()) {
      CameraCharacteristics characteristics = manager.getCameraCharacteristics(cameraId);
      Integer facing = characteristics.get(CameraCharacteristics.LENS_FACING);
      if (facing == null || facing != CameraCharacteristics.LENS_FACING_BACK) {
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
      for (Range<Integer> range : streams.getHighSpeedVideoFpsRangesFor(requestedSize)) {
        if (range.getLower() == request.framesPerSecond()
            && range.getUpper() == request.framesPerSecond()) {
          Integer timestampSource =
              characteristics.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE);
          return new CameraSelection(
              cameraId, timestampSource == null ? -1 : timestampSource);
        }
      }
    }
    throw new IllegalStateException(
        "No rear camera supports "
            + request.width()
            + "x"
            + request.height()
            + " at fixed "
            + request.framesPerSecond()
            + " fps");
  }

  private static JSONArray acceptanceFailures(
      Request request,
      CameraSelection selection,
      CameraCapture camera,
      EncoderCapture encoder,
      CameraEncoderTimestampAlignment.Result timestampAlignment,
      AudioCapture audio,
      Throwable fatalFailure,
      long windowStartNanos,
      long windowStopNanos,
      long audioFramesAtStart,
      long audioFramesAtStop) {
    JSONArray failures = new JSONArray();
    if (fatalFailure != null) {
      failures.put("fatal_error");
    }
    long expectedVideoFrames =
        Math.round(
            (windowStopNanos - windowStartNanos)
                * request.framesPerSecond()
                / 1_000_000_000.0);
    if (selection == null
        || selection.timestampSource
            != CameraMetadata.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME) {
      failures.put("camera_timestamp_not_realtime");
    }
    if (camera == null || camera.failure() != null) {
      failures.put("camera_failure");
    } else if (camera.sensorTimestamps.count() == 0) {
      failures.put("no_camera_results");
    } else if (camera.sensorTimestamps.nonmonotonicCount() != 0) {
      failures.put("nonmonotonic_sensor_timestamps");
    }
    if (encoder == null || encoder.failure() != null) {
      failures.put("encoder_failure");
    } else if (!timestampAlignment.valid()) {
      failures.put("encoder_sensor_timestamp_alignment_invalid");
    } else {
      ProbeStatistics window =
          encoder.presentationTimestamps(windowStartNanos, windowStopNanos, timestampAlignment);
      if (!encoder.eosObserved()) {
        failures.put("encoder_eos_missing");
      }
      if (window.distinctCount() < expectedVideoFrames * 95 / 100) {
        failures.put("too_few_encoded_frames");
      }
      double rate = window.measuredRate(1_000_000.0);
      if (rate < request.framesPerSecond() * 0.97
          || rate > request.framesPerSecond() * 1.03) {
        failures.put("encoded_frame_rate_out_of_range");
      }
      if (window.nonmonotonicCount() != 0) {
        failures.put("nonmonotonic_encoder_timestamps");
      }
      if (window.duplicateCount() != 0) {
        failures.put("duplicate_encoder_timestamps");
      }
      double nominalDeltaUs = 1_000_000.0 / request.framesPerSecond();
      if (window.maximumPositiveDelta() > Math.ceil(nominalDeltaUs * 1.5)) {
        failures.put("encoder_timestamp_gap");
      }
    }
    long expectedAudioFrames =
        Math.round(
            (windowStopNanos - windowStartNanos)
                * AUDIO_SAMPLE_RATE_HZ
                / 1_000_000_000.0);
    if (audio == null || audio.failure() != null) {
      failures.put("audio_failure");
    } else {
      long windowFrames = audioFramesAtStop - audioFramesAtStart;
      if (windowFrames < expectedAudioFrames * 95 / 100) {
        failures.put("too_few_audio_frames");
      }
      AudioClockWindow audioClock = audio.clockWindow(windowStartNanos, windowStopNanos);
      if (audioClock.pairCount < 2) {
        failures.put("insufficient_audio_timestamps");
      } else if (audioClock.measuredSampleRateHz < AUDIO_SAMPLE_RATE_HZ * 0.99
          || audioClock.measuredSampleRateHz > AUDIO_SAMPLE_RATE_HZ * 1.01) {
        failures.put("audio_clock_rate_out_of_range");
      }
    }
    return failures;
  }

  private static JSONObject timingJson(ProbeStatistics statistics, double unitsPerSecond)
      throws Exception {
    JSONObject json = new JSONObject();
    json.put("count", statistics.count());
    json.put("distinct_count", statistics.distinctCount());
    json.put("first", statistics.first());
    json.put("last", statistics.last());
    json.put("span", statistics.span());
    json.put("minimum_positive_delta", statistics.minimumPositiveDelta());
    json.put("maximum_positive_delta", statistics.maximumPositiveDelta());
    json.put("duplicate_count", statistics.duplicateCount());
    json.put("nonmonotonic_count", statistics.nonmonotonicCount());
    json.put("measured_rate_hz", statistics.measuredRate(unitsPerSecond));
    return json;
  }

  private static JSONObject timestampAlignmentJson(
      CameraEncoderTimestampAlignment.Result alignment) throws Exception {
    JSONObject json = new JSONObject();
    json.put("valid", alignment.valid());
    json.put("diagnostic", alignment.diagnostic());
    json.put("mapping", "camera2_frame_number_to_encoder_output_ordinal");
    json.put("pair_count", alignment.pairCount());
    json.put("first_matched_frame_number", alignment.firstMatchedFrameNumber());
    json.put("last_matched_frame_number", alignment.lastMatchedFrameNumber());
    json.put("complete_ordinal_coverage", alignment.completeOrdinalCoverage());
    json.put(
        "encoder_to_sensor_offset_ns",
        Long.toString(alignment.encoderToSensorOffsetNanos()));
    json.put("offset_span_ns", alignment.offsetSpanNanos());
    return json;
  }

  private static JSONObject publishRetainedSession(
      Context context,
      CaptureConfigurationSnapshot captureConfiguration,
      Request request,
      CameraSelection selection,
      EncoderCapture encoder,
      CameraEncoderTimestampAlignment.Result timestampAlignment,
      long triggerNanos) throws Exception {
    List<EncodedSample> samples = encoder.retainedSamples();
    if (samples.isEmpty()) {
      throw new IllegalStateException("No encoded samples were retained");
    }
    MediaFormat format = encoder.outputMediaFormat();
    if (format == null) {
      throw new IllegalStateException("Encoder output format is unavailable");
    }
    AvcCodecDescriptor codec = AvcMediaFormat.describe(format);

    long triggerUs = timestampAlignment.encoderPresentationTimeUs(triggerNanos);
    long desiredStartUs = triggerUs - 1_400_000;
    long desiredStopUs = triggerUs + 500_000;
    int startIndex = -1;
    for (int index = 0; index < samples.size(); ++index) {
      EncodedSample sample = samples.get(index);
      if (sample.presentationTimeUs > desiredStartUs) {
        break;
      }
      if (sample.isKeyFrame()) {
        startIndex = index;
      }
    }
    if (startIndex < 0) {
      throw new IllegalStateException("No keyframe precedes the desired retained window");
    }
    int stopIndexExclusive = startIndex;
    while (stopIndexExclusive < samples.size()
        && samples.get(stopIndexExclusive).presentationTimeUs <= desiredStopUs) {
      ++stopIndexExclusive;
    }
    if (stopIndexExclusive - startIndex < 2) {
      throw new IllegalStateException("Retained window does not contain enough encoded samples");
    }
    List<EncodedSample> selected =
        new ArrayList<>(samples.subList(startIndex, stopIndexExclusive));
    if (!selected.get(0).isKeyFrame()) {
      throw new IllegalStateException("Retained media does not start on a keyframe");
    }

    String sessionId =
        "android-"
            + System.currentTimeMillis()
            + "-"
            + UUID.randomUUID().toString().substring(0, 8);
    File sessions = new File(context.getFilesDir(), "sessions");
    if (!sessions.isDirectory() && !sessions.mkdirs()) {
      throw new IllegalStateException("Unable to create sessions directory " + sessions);
    }
    File temporaryDirectory = new File(sessions, sessionId + ".tmp");
    if (!temporaryDirectory.mkdir()) {
      throw new IllegalStateException(
          "Unable to create temporary session directory " + temporaryDirectory);
    }
    String mediaName = captureConfiguration.mediaFileName();
    File temporaryMedia = new File(temporaryDirectory, mediaName + ".tmp");
    File media = new File(temporaryDirectory, mediaName);

    MediaMuxer muxer =
        new MediaMuxer(temporaryMedia.getAbsolutePath(), MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
    boolean muxerStarted = false;
    try {
      int trackIndex = muxer.addTrack(format);
      muxer.start();
      muxerStarted = true;
      long firstPresentationTimeUs = selected.get(0).presentationTimeUs;
      for (EncodedSample sample : selected) {
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        info.set(
            0,
            sample.bytes.length,
            sample.presentationTimeUs - firstPresentationTimeUs,
            sample.flags);
        muxer.writeSampleData(trackIndex, ByteBuffer.wrap(sample.bytes), info);
      }
      muxer.stop();
      muxerStarted = false;
    } finally {
      if (muxerStarted) {
        try {
          muxer.stop();
        } catch (Throwable ignored) {
          // Preserve the primary muxing failure.
        }
      }
      muxer.release();
    }
    try (FileOutputStream sync = new FileOutputStream(temporaryMedia, true)) {
      sync.getFD().sync();
    }
    if (!temporaryMedia.renameTo(media)) {
      throw new IllegalStateException("Unable to publish retained media " + media);
    }
    long encodedMediaBytes = media.length();

    long firstPtsUs = selected.get(0).presentationTimeUs;
    long lastPtsUs = selected.get(selected.size() - 1).presentationTimeUs;
    int impactFrameIndex = 0;
    long closestImpactDistance = Long.MAX_VALUE;
    JSONArray frames = new JSONArray();
    for (int index = 0; index < selected.size(); ++index) {
      EncodedSample sample = selected.get(index);
      long impactDistance = Math.abs(sample.presentationTimeUs - triggerUs);
      if (impactDistance < closestImpactDistance) {
        closestImpactDistance = impactDistance;
        impactFrameIndex = index;
      }
      JSONObject frame = new JSONObject();
      frame.put("frame_index", index);
      frame.put("frame_id", Long.toString(sample.ordinal));
      frame.put(
          "device_timestamp",
          Long.toString(timestampAlignment.sensorTimestampNanos(sample.presentationTimeUs)));
      frame.put("time_from_impact_us", sample.presentationTimeUs - triggerUs);
      frame.put("media_time_us", sample.presentationTimeUs - firstPtsUs);
      frames.put(frame);
    }
    SingleViewManifestTiming manifestTiming =
        SingleViewManifestTiming.fromAbsoluteDeltaUs(closestImpactDistance);

    JSONObject track = new JSONObject();
    track.put("role", captureConfiguration.role().wireName());
    track.put("camera_serial", captureConfiguration.nodeId());
    track.put(
        "source",
        new JSONObject()
            .put("pixel_format", "camera2_private")
            .put("width", request.width())
            .put("height", request.height()));
    track.put(
        "encoded",
        new JSONObject().put("width", request.width()).put("height", request.height()));
    track.put("frame_count", selected.size());
    track.put("nominal_fps", request.framesPerSecond());
    track.put("impact_frame_index", impactFrameIndex);
    track.put(
        "media",
        new JSONObject()
            .put("path", mediaName)
            .put("mime_type", "video/mp4")
            .put("codec", codec.rfc6381Codec())
            .put("all_frames_keyframes", false)
            .put("encoded_bytes", encodedMediaBytes));
    track.put("frames", frames);

    JSONObject manifest = new JSONObject();
    manifest.put("schema_version", 1);
    manifest.put("session_id", sessionId);
    manifest.put("created_at_utc", Instant.now().toString());
    manifest.put(
        "trigger",
        new JSONObject()
            .put("source", "synthetic_probe")
            .put("host_monotonic_time_ns", Long.toString(triggerNanos))
            .put("confirmation_host_monotonic_time_ns", Long.toString(triggerNanos))
            .put("sample_rate_hz", JSONObject.NULL)
            .put("peak_amplitude", JSONObject.NULL)
            .put("noise_floor", JSONObject.NULL)
            .put("threshold", JSONObject.NULL));
    manifest.put(
        SingleViewManifestTiming.INTER_VIEW_SKEW_FIELD,
        manifestTiming.mappedNearestFrameSkewUs() == null
            ? JSONObject.NULL
            : manifestTiming.mappedNearestFrameSkewUs());
    manifest.put("views", new JSONArray().put(track));
    manifest.put(
        "android_capture",
        new JSONObject()
            .put("node_id", captureConfiguration.nodeId())
            .put("camera_id", selection.cameraId)
            .put("camera_timestamp_source", selection.timestampSource)
            .put("avc_profile_idc", codec.profileIdc())
            .put("avc_profile_compatibility", codec.profileCompatibility())
            .put("avc_level_idc", codec.levelIdc())
            .put("timestamp_mapping", "camera2_frame_number_to_encoder_output_ordinal")
            .put(
                "encoder_to_sensor_offset_ns",
                Long.toString(timestampAlignment.encoderToSensorOffsetNanos()))
            .put("timestamp_offset_span_ns", timestampAlignment.offsetSpanNanos())
            .put("timestamp_pair_count", timestampAlignment.pairCount())
            .put(
                SingleViewManifestTiming.LOCAL_RESIDUAL_FIELD,
                manifestTiming.localNearestFrameResidualUs())
            .put("requested_pre_roll_us", 1_400_000)
            .put("requested_post_roll_us", 500_000)
            .put("actual_pre_roll_us", triggerUs - firstPtsUs)
            .put("actual_post_roll_us", lastPtsUs - triggerUs)
            .put("retained_start_extended_to_keyframe", firstPtsUs < desiredStartUs));

    File temporaryManifest = new File(temporaryDirectory, "manifest.json.tmp");
    File publishedManifest = new File(temporaryDirectory, "manifest.json");
    try (FileOutputStream output = new FileOutputStream(temporaryManifest)) {
      output.write((manifest.toString(2) + "\n").getBytes(StandardCharsets.UTF_8));
      output.getFD().sync();
    }
    if (!temporaryManifest.renameTo(publishedManifest)) {
      throw new IllegalStateException("Unable to publish retained manifest " + publishedManifest);
    }
    File publishedDirectory = new File(sessions, sessionId);
    if (!temporaryDirectory.renameTo(publishedDirectory)) {
      throw new IllegalStateException("Unable to atomically publish session " + sessionId);
    }

    JSONObject result = new JSONObject();
    result.put("session_id", sessionId);
    result.put("relative_directory", "sessions/" + sessionId);
    result.put("manifest", "sessions/" + sessionId + "/manifest.json");
    result.put("media", "sessions/" + sessionId + "/" + mediaName);
    result.put("frame_count", selected.size());
    result.put("encoded_bytes", encodedMediaBytes);
    result.put("impact_frame_index", impactFrameIndex);
    result.put("actual_pre_roll_us", triggerUs - firstPtsUs);
    result.put("actual_post_roll_us", lastPtsUs - triggerUs);
    return result;
  }

  private static String describe(Throwable failure) {
    String message = failure.getMessage();
    return failure.getClass().getName() + (message == null ? "" : ": " + message);
  }

  private static final class CameraSelection {
    private final String cameraId;
    private final int timestampSource;

    private CameraSelection(String cameraId, int timestampSource) {
      this.cameraId = cameraId;
      this.timestampSource = timestampSource;
    }
  }

  private static final class CameraCapture {
    private final CameraManager manager;
    private final String cameraId;
    private final Request request;
    private final Surface surface;
    private final HandlerThread handlerThread = new HandlerThread("high-speed-camera");
    private final CountDownLatch firstCapture = new CountDownLatch(1);
    private final AtomicReference<Throwable> failure = new AtomicReference<>();
    private final ProbeStatistics sensorTimestamps = new ProbeStatistics();
    private final ProbeStatistics resultFrameNumbers = new ProbeStatistics();
    private final List<CameraEncoderTimestampAlignment.CameraSample> timestampSamples =
        new ArrayList<>();
    private volatile CameraDevice device;
    private volatile CameraConstrainedHighSpeedCaptureSession session;

    private CameraCapture(Context context, String cameraId, Request request, Surface surface) {
      manager = context.getSystemService(CameraManager.class);
      this.cameraId = cameraId;
      this.request = request;
      this.surface = surface;
    }

    private void start() throws Exception {
      handlerThread.start();
      Handler handler = new Handler(handlerThread.getLooper());
      manager.openCamera(
          cameraId,
          new CameraDevice.StateCallback() {
            @Override
            public void onOpened(CameraDevice openedDevice) {
              device = openedDevice;
              createSession(openedDevice, handler);
            }

            @Override
            public void onDisconnected(CameraDevice disconnectedDevice) {
              fail(new IllegalStateException("Camera disconnected"));
              disconnectedDevice.close();
            }

            @Override
            public void onError(CameraDevice errorDevice, int errorCode) {
              fail(new IllegalStateException("Camera error " + errorCode));
              errorDevice.close();
            }
          },
          handler);
      if (!firstCapture.await(START_TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
        Throwable startFailure = failure.get();
        if (startFailure != null) {
          throw new IllegalStateException("High-speed capture failed to start", startFailure);
        }
        throw new IllegalStateException("Timed out waiting for the first high-speed frame");
      }
      Throwable startFailure = failure.get();
      if (startFailure != null) {
        throw new IllegalStateException("High-speed capture failed to start", startFailure);
      }
    }

    @SuppressWarnings("deprecation")
    private void createSession(CameraDevice openedDevice, Handler handler) {
      try {
        CaptureRequest.Builder requestBuilder =
            openedDevice.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
        requestBuilder.addTarget(surface);
        requestBuilder.set(
            CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE,
            new Range<>(request.framesPerSecond(), request.framesPerSecond()));
        requestBuilder.set(
            CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
        openedDevice.createConstrainedHighSpeedCaptureSession(
            Collections.singletonList(surface),
            new CameraCaptureSession.StateCallback() {
              @Override
              public void onConfigured(CameraCaptureSession configuredSession) {
                session = (CameraConstrainedHighSpeedCaptureSession) configuredSession;
                try {
                  List<CaptureRequest> burst =
                      session.createHighSpeedRequestList(requestBuilder.build());
                  session.setRepeatingBurst(
                      burst,
                      new CameraCaptureSession.CaptureCallback() {
                        @Override
                        public void onCaptureStarted(
                            CameraCaptureSession ignoredSession,
                            CaptureRequest ignoredRequest,
                            long ignoredTimestamp,
                            long ignoredFrameNumber) {
                          firstCapture.countDown();
                        }

                        @Override
                        public void onCaptureCompleted(
                            CameraCaptureSession ignoredSession,
                            CaptureRequest ignoredRequest,
                            TotalCaptureResult result) {
                          Long timestamp = result.get(CaptureResult.SENSOR_TIMESTAMP);
                          if (timestamp != null) {
                            sensorTimestamps.add(timestamp);
                            synchronized (timestampSamples) {
                              timestampSamples.add(
                                  new CameraEncoderTimestampAlignment.CameraSample(
                                      result.getFrameNumber(), timestamp));
                            }
                          }
                          resultFrameNumbers.add(result.getFrameNumber());
                        }

                        @Override
                        public void onCaptureFailed(
                            CameraCaptureSession ignoredSession,
                            CaptureRequest ignoredRequest,
                            android.hardware.camera2.CaptureFailure captureFailure) {
                          fail(
                              new IllegalStateException(
                                  "Capture request failed: " + captureFailure.getReason()));
                        }
                      },
                      handler);
                } catch (Throwable sessionFailure) {
                  fail(sessionFailure);
                }
              }

              @Override
              public void onConfigureFailed(CameraCaptureSession ignoredSession) {
                fail(new IllegalStateException("High-speed session configuration failed"));
              }
            },
            handler);
      } catch (Throwable sessionFailure) {
        fail(sessionFailure);
      }
    }

    private void fail(Throwable cameraFailure) {
      failure.compareAndSet(null, cameraFailure);
      firstCapture.countDown();
    }

    private Throwable failure() {
      return failure.get();
    }

    private void recordFailure(Throwable cameraFailure) {
      failure.compareAndSet(null, cameraFailure);
    }

    private List<CameraEncoderTimestampAlignment.CameraSample> timestampSamples() {
      synchronized (timestampSamples) {
        return new ArrayList<>(timestampSamples);
      }
    }

    private void stop() {
      CameraConstrainedHighSpeedCaptureSession currentSession = session;
      if (currentSession != null) {
        try {
          currentSession.stopRepeating();
          currentSession.abortCaptures();
        } catch (Throwable stopFailure) {
          failure.compareAndSet(null, stopFailure);
        }
        try {
          currentSession.close();
        } catch (Throwable closeFailure) {
          failure.compareAndSet(null, closeFailure);
        }
      }
      CameraDevice currentDevice = device;
      if (currentDevice != null) {
        try {
          currentDevice.close();
        } catch (Throwable closeFailure) {
          failure.compareAndSet(null, closeFailure);
        }
      }
      handlerThread.quitSafely();
      try {
        handlerThread.join(TimeUnit.SECONDS.toMillis(THREAD_STOP_TIMEOUT_SECONDS));
        if (handlerThread.isAlive()) {
          failure.compareAndSet(null, new IllegalStateException("Camera thread did not stop"));
        }
      } catch (InterruptedException interrupted) {
        Thread.currentThread().interrupt();
        failure.compareAndSet(null, interrupted);
      }
    }

    private JSONObject toJson() throws Exception {
      JSONObject json = new JSONObject();
      json.put("sensor_timestamp_units", "nanoseconds");
      json.put("sensor_timestamps", timingJson(sensorTimestamps, 1_000_000_000.0));
      json.put("result_frame_numbers", timingJson(resultFrameNumbers, 1.0));
      Throwable cameraFailure = failure();
      if (cameraFailure != null) {
        json.put("error", describe(cameraFailure));
      }
      return json;
    }
  }

  private static final class EncodedSample {
    private final long ordinal;
    private final long presentationTimeUs;
    private final int flags;
    private final byte[] bytes;

    private EncodedSample(long ordinal, long presentationTimeUs, int flags, byte[] bytes) {
      this.ordinal = ordinal;
      this.presentationTimeUs = presentationTimeUs;
      this.flags = flags;
      this.bytes = bytes;
    }

    private boolean isKeyFrame() {
      return (flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
    }
  }

  private static final class EncoderCapture {
    private final Request request;
    private final boolean retainSamples;
    private final AtomicReference<Throwable> failure = new AtomicReference<>();
    private final AtomicBoolean stopRequested = new AtomicBoolean();
    private final AtomicBoolean eosObserved = new AtomicBoolean();
    private final AtomicLong encodedBytes = new AtomicLong();
    private final ProbeStatistics presentationTimestamps = new ProbeStatistics();
    private final List<Long> presentationTimesUs = new ArrayList<>();
    private final List<EncodedSample> encodedSamples = new ArrayList<>();
    private MediaCodec codec;
    private Surface inputSurface;
    private Thread drainThread;
    private String codecName = "";
    private String outputFormat = "";
    private MediaFormat outputMediaFormat;
    private long sampleOrdinal;

    private EncoderCapture(Request request, boolean retainSamples) {
      this.request = request;
      this.retainSamples = retainSamples;
    }

    private void start() throws Exception {
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
      codec.start();
      drainThread = new Thread(this::drain, "video-encoder-drain");
      drainThread.start();
    }

    private Surface inputSurface() {
      return inputSurface;
    }

    private void drain() {
      MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
      long idleDeadlineNanos = Long.MAX_VALUE;
      try {
        while (true) {
          int index = codec.dequeueOutputBuffer(info, 20_000);
          if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
            outputMediaFormat = codec.getOutputFormat();
            outputFormat = outputMediaFormat.toString();
          } else if (index >= 0) {
            if (info.size > 0 && (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
              encodedBytes.addAndGet(info.size);
              presentationTimestamps.add(info.presentationTimeUs);
              synchronized (presentationTimesUs) {
                presentationTimesUs.add(info.presentationTimeUs);
              }
              if (retainSamples) {
                ByteBuffer output = codec.getOutputBuffer(index);
                if (output == null) {
                  throw new IllegalStateException("Encoder returned a null output buffer");
                }
                ByteBuffer selected = output.duplicate();
                selected.position(info.offset);
                selected.limit(info.offset + info.size);
                byte[] bytes = new byte[info.size];
                selected.get(bytes);
                synchronized (encodedSamples) {
                  encodedSamples.add(
                      new EncodedSample(
                          sampleOrdinal++, info.presentationTimeUs, info.flags, bytes));
                }
              }
            }
            boolean endOfStream = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
            codec.releaseOutputBuffer(index, false);
            if (endOfStream) {
              eosObserved.set(true);
              return;
            }
          }
          if (stopRequested.get()) {
            if (idleDeadlineNanos == Long.MAX_VALUE) {
              idleDeadlineNanos = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
            } else if (System.nanoTime() >= idleDeadlineNanos) {
              failure.compareAndSet(
                  null, new IllegalStateException("Encoder EOS was not observed"));
              return;
            }
          }
        }
      } catch (Throwable drainFailure) {
        failure.compareAndSet(null, drainFailure);
      }
    }

    private void stop() {
      stopRequested.set(true);
      if (codec == null) {
        return;
      }
      try {
        codec.signalEndOfInputStream();
      } catch (Throwable signalFailure) {
        failure.compareAndSet(null, signalFailure);
      }
      if (drainThread != null) {
        try {
          drainThread.join(TimeUnit.SECONDS.toMillis(THREAD_STOP_TIMEOUT_SECONDS));
          if (drainThread.isAlive()) {
            failure.compareAndSet(null, new IllegalStateException("Encoder drain did not stop"));
            return;
          }
        } catch (InterruptedException interrupted) {
          Thread.currentThread().interrupt();
          failure.compareAndSet(null, interrupted);
        }
      }
      try {
        codec.stop();
      } catch (Throwable stopFailure) {
        failure.compareAndSet(null, stopFailure);
      } finally {
        try {
          codec.release();
        } catch (Throwable releaseFailure) {
          failure.compareAndSet(null, releaseFailure);
        }
        if (inputSurface != null) {
          try {
            inputSurface.release();
          } catch (Throwable releaseFailure) {
            failure.compareAndSet(null, releaseFailure);
          }
        }
      }
    }

    private Throwable failure() {
      return failure.get();
    }

    private void recordFailure(Throwable encoderFailure) {
      failure.compareAndSet(null, encoderFailure);
    }

    private boolean eosObserved() {
      return eosObserved.get();
    }

    private List<EncodedSample> retainedSamples() {
      synchronized (encodedSamples) {
        return new ArrayList<>(encodedSamples);
      }
    }

    private MediaFormat outputMediaFormat() {
      return outputMediaFormat;
    }

    private List<Long> presentationTimesUs() {
      synchronized (presentationTimesUs) {
        return new ArrayList<>(presentationTimesUs);
      }
    }

    private ProbeStatistics presentationTimestamps(
        long windowStartNanos,
        long windowStopNanos,
        CameraEncoderTimestampAlignment.Result timestampAlignment) {
      ProbeStatistics window = new ProbeStatistics();
      synchronized (presentationTimesUs) {
        for (long timestampUs : presentationTimesUs) {
          long sensorTimestampNanos = timestampAlignment.sensorTimestampNanos(timestampUs);
          if (sensorTimestampNanos >= windowStartNanos && sensorTimestampNanos < windowStopNanos) {
            window.add(timestampUs);
          }
        }
      }
      return window;
    }

    private JSONObject toJson(
        long windowStartNanos,
        long windowStopNanos,
        CameraEncoderTimestampAlignment.Result timestampAlignment) throws Exception {
      JSONObject json = new JSONObject();
      json.put("codec", codecName);
      json.put("output_format", outputFormat);
      json.put("encoded_bytes", encodedBytes.get());
      json.put("eos_observed", eosObserved());
      json.put("presentation_timestamp_units", "microseconds");
      json.put(
          "presentation_timestamps", timingJson(presentationTimestamps, 1_000_000.0));
      json.put(
          "acceptance_window_presentation_timestamps",
          timestampAlignment.valid()
              ? timingJson(
                  presentationTimestamps(windowStartNanos, windowStopNanos, timestampAlignment),
                  1_000_000.0)
              : JSONObject.NULL);
      json.put("timestamp_alignment", timestampAlignmentJson(timestampAlignment));
      Throwable encoderFailure = failure();
      if (encoderFailure != null) {
        json.put("error", describe(encoderFailure));
      }
      return json;
    }
  }

  private static String selectEncoder(Request request) {
    for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.ALL_CODECS).getCodecInfos()) {
      if (!info.isEncoder() || !info.isHardwareAccelerated()) {
        continue;
      }
      for (String supportedType : info.getSupportedTypes()) {
        if (!request.mime().equalsIgnoreCase(supportedType)) {
          continue;
        }
        try {
          MediaCodecInfo.VideoCapabilities capabilities =
              info.getCapabilitiesForType(supportedType).getVideoCapabilities();
          if (capabilities.areSizeAndRateSupported(
              request.width(), request.height(), request.framesPerSecond())) {
            return info.getName();
          }
        } catch (IllegalArgumentException ignored) {
          // Continue to the next candidate.
        }
      }
    }
    throw new IllegalStateException(
        "No hardware "
            + request.mime()
            + " encoder supports "
            + request.width()
            + "x"
            + request.height()
            + " at "
            + request.framesPerSecond()
            + " fps");
  }

  private static final class AudioCapture {
    private final AudioManager audioManager;
    private final AtomicReference<Throwable> failure = new AtomicReference<>();
    private final AtomicBoolean stopRequested = new AtomicBoolean();
    private final AtomicLong framesRead = new AtomicLong();
    private final AtomicLong timestampFramePositionFirst = new AtomicLong(-1);
    private final AtomicLong timestampFramePositionLast = new AtomicLong(-1);
    private final ProbeStatistics timestamps = new ProbeStatistics();
    private final List<AudioTimestampPair> timestampPairs = new ArrayList<>();
    private final CountDownLatch recordingStarted = new CountDownLatch(1);
    private AudioRecord record;
    private Thread thread;
    private int bufferBytes;
    private int audioSource;

    private AudioCapture(Context context) {
      audioManager = context.getSystemService(AudioManager.class);
    }

    private void start() throws Exception {
      int minimumBuffer =
          AudioRecord.getMinBufferSize(
              AUDIO_SAMPLE_RATE_HZ,
              AudioFormat.CHANNEL_IN_MONO,
              AudioFormat.ENCODING_PCM_16BIT);
      if (minimumBuffer <= 0) {
        throw new IllegalStateException("Invalid minimum AudioRecord buffer " + minimumBuffer);
      }
      bufferBytes = Math.max(minimumBuffer * 4, AUDIO_SAMPLE_RATE_HZ / 5 * 2);
      audioSource =
          Boolean.parseBoolean(
                  audioManager.getProperty(
                      AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED))
              ? MediaRecorder.AudioSource.UNPROCESSED
              : MediaRecorder.AudioSource.VOICE_RECOGNITION;
      record =
          new AudioRecord.Builder()
              .setAudioSource(audioSource)
              .setAudioFormat(
                  new AudioFormat.Builder()
                      .setSampleRate(AUDIO_SAMPLE_RATE_HZ)
                      .setChannelMask(AudioFormat.CHANNEL_IN_MONO)
                      .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                      .build())
              .setBufferSizeInBytes(bufferBytes)
              .build();
      if (record.getState() != AudioRecord.STATE_INITIALIZED) {
        record.release();
        throw new IllegalStateException("AudioRecord did not initialize");
      }
      thread = new Thread(this::recordAudio, "audio-probe");
      thread.start();
      if (!recordingStarted.await(START_TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
        throw new IllegalStateException("Timed out starting AudioRecord");
      }
      Throwable startFailure = failure.get();
      if (startFailure != null) {
        throw new IllegalStateException("AudioRecord failed to start", startFailure);
      }
    }

    private void recordAudio() {
      byte[] buffer = new byte[bufferBytes];
      AudioTimestamp timestamp = new AudioTimestamp();
      try {
        record.startRecording();
        if (record.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) {
          throw new IllegalStateException("AudioRecord is not recording");
        }
        recordingStarted.countDown();
        while (!stopRequested.get()) {
          int bytes = record.read(buffer, 0, buffer.length, AudioRecord.READ_BLOCKING);
          if (bytes <= 0 && stopRequested.get()) {
            return;
          }
          if (bytes < 0) {
            throw new IllegalStateException("AudioRecord read failed with " + bytes);
          }
          framesRead.addAndGet(bytes / 2);
          if (record.getTimestamp(timestamp, AudioTimestamp.TIMEBASE_BOOTTIME)
              == AudioRecord.SUCCESS) {
            timestampFramePositionFirst.compareAndSet(-1, timestamp.framePosition);
            timestampFramePositionLast.set(timestamp.framePosition);
            timestamps.add(timestamp.nanoTime);
            synchronized (timestampPairs) {
              timestampPairs.add(
                  new AudioTimestampPair(timestamp.framePosition, timestamp.nanoTime));
            }
          }
        }
      } catch (Throwable audioFailure) {
        failure.compareAndSet(null, audioFailure);
        recordingStarted.countDown();
      }
    }

    private void stop() {
      stopRequested.set(true);
      if (record == null) {
        return;
      }
      try {
        record.stop();
      } catch (Throwable stopFailure) {
        failure.compareAndSet(null, stopFailure);
      }
      if (thread != null) {
        try {
          thread.join(TimeUnit.SECONDS.toMillis(THREAD_STOP_TIMEOUT_SECONDS));
          if (thread.isAlive()) {
            failure.compareAndSet(null, new IllegalStateException("Audio thread did not stop"));
            return;
          }
        } catch (InterruptedException interrupted) {
          Thread.currentThread().interrupt();
          failure.compareAndSet(null, interrupted);
        }
      }
      record.release();
    }

    private Throwable failure() {
      return failure.get();
    }

    private void recordFailure(Throwable audioFailure) {
      failure.compareAndSet(null, audioFailure);
    }

    private long framesRead() {
      return framesRead.get();
    }

    private AudioClockWindow clockWindow(long windowStartNanos, long windowStopNanos) {
      List<AudioTimestampPair> selected = new ArrayList<>();
      synchronized (timestampPairs) {
        for (AudioTimestampPair pair : timestampPairs) {
          if (pair.nanoTime >= windowStartNanos && pair.nanoTime < windowStopNanos) {
            selected.add(pair);
          }
        }
      }
      if (selected.size() < 2) {
        return new AudioClockWindow(selected.size(), 0, 0, 0.0);
      }
      AudioTimestampPair first = selected.get(0);
      AudioTimestampPair last = selected.get(selected.size() - 1);
      long frameSpan = last.framePosition - first.framePosition;
      long timeSpanNanos = last.nanoTime - first.nanoTime;
      double measuredRate =
          frameSpan > 0 && timeSpanNanos > 0
              ? frameSpan * 1_000_000_000.0 / timeSpanNanos
              : 0.0;
      return new AudioClockWindow(selected.size(), frameSpan, timeSpanNanos, measuredRate);
    }

    private JSONObject toJson(
        long windowStartNanos,
        long windowStopNanos,
        long framesAtStart,
        long framesAtStop) throws Exception {
      JSONObject json = new JSONObject();
      json.put("sample_rate_hz", AUDIO_SAMPLE_RATE_HZ);
      json.put("channel_count", 1);
      json.put("encoding", "pcm_16bit");
      json.put(
          "audio_source",
          audioSource == MediaRecorder.AudioSource.UNPROCESSED
              ? "unprocessed"
              : "voice_recognition");
      json.put("timestamp_timebase", "boottime");
      json.put("buffer_bytes", bufferBytes);
      json.put("frames_read", framesRead.get());
      json.put("acceptance_window_frames_read", framesAtStop - framesAtStart);
      json.put("timestamp_frame_position_first", timestampFramePositionFirst.get());
      json.put("timestamp_frame_position_last", timestampFramePositionLast.get());
      json.put("timestamps", timingJson(timestamps, 1_000_000_000.0));
      json.put("acceptance_clock", clockWindow(windowStartNanos, windowStopNanos).toJson());
      Throwable audioFailure = failure();
      if (audioFailure != null) {
        json.put("error", describe(audioFailure));
      }
      return json;
    }
  }

  private static final class AudioTimestampPair {
    private final long framePosition;
    private final long nanoTime;

    private AudioTimestampPair(long framePosition, long nanoTime) {
      this.framePosition = framePosition;
      this.nanoTime = nanoTime;
    }
  }

  private static final class AudioClockWindow {
    private final int pairCount;
    private final long framePositionSpan;
    private final long timeSpanNanos;
    private final double measuredSampleRateHz;

    private AudioClockWindow(
        int pairCount,
        long framePositionSpan,
        long timeSpanNanos,
        double measuredSampleRateHz) {
      this.pairCount = pairCount;
      this.framePositionSpan = framePositionSpan;
      this.timeSpanNanos = timeSpanNanos;
      this.measuredSampleRateHz = measuredSampleRateHz;
    }

    private JSONObject toJson() throws Exception {
      JSONObject json = new JSONObject();
      json.put("pair_count", pairCount);
      json.put("frame_position_span", framePositionSpan);
      json.put("time_span_ns", timeSpanNanos);
      json.put("measured_sample_rate_hz", measuredSampleRateHz);
      return json;
    }
  }
}
