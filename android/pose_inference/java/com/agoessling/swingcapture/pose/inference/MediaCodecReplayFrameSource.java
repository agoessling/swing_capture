package com.agoessling.swingcapture.pose.inference;

import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.view.Surface;
import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.Objects;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Pull-based MediaCodec replay for bounded clips stored under an app-private root. */
public final class MediaCodecReplayFrameSource implements PoseFrameSource {
  private static final long MAX_CLIP_BYTES = 512L * 1024L * 1024L;
  private static final long MAX_DURATION_US = 10L * 60L * 1_000_000L;
  private static final long CODEC_TIMEOUT_US = 10_000L;
  private static final long IMAGE_TIMEOUT_MILLIS = 2_000L;
  private static final int MAXIMUM_ACQUIRED_IMAGES = 2;

  /** Bounded decoder-stage telemetry emitted by {@link PoseReplayHilRunner}. */
  public record PerformanceSnapshot(
      String decoderName,
      String outputTransport,
      long nextFrameCalls,
      long outputBuffers,
      long skippedOutputBuffers,
      long selectedFrames,
      long totalDequeueNanos,
      long totalImageWaitNanos,
      long totalConversionNanos,
      long totalNextFrameNanos) {
    public PerformanceSnapshot {
      Objects.requireNonNull(decoderName, "decoderName");
      Objects.requireNonNull(outputTransport, "outputTransport");
      if (decoderName.isEmpty()
          || decoderName.length() > 200
          || nextFrameCalls < 0
          || outputBuffers < 0
          || skippedOutputBuffers < 0
          || selectedFrames < 0
          || totalDequeueNanos < 0
          || totalImageWaitNanos < 0
          || totalConversionNanos < 0
          || totalNextFrameNanos < 0
          || skippedOutputBuffers > outputBuffers
          || selectedFrames > outputBuffers) {
        throw new IllegalArgumentException("replay decoder performance metrics are invalid");
      }
    }

    public String toLogString() {
      return "decoder="
          + decoderName
          + " output_transport="
          + outputTransport
          + " next_frame_calls="
          + nextFrameCalls
          + " output_buffers="
          + outputBuffers
          + " skipped_output_buffers="
          + skippedOutputBuffers
          + " selected_frames="
          + selectedFrames
          + " dequeue_ms="
          + nanosToMillis(totalDequeueNanos)
          + " image_wait_ms="
          + nanosToMillis(totalImageWaitNanos)
          + " conversion_ms="
          + nanosToMillis(totalConversionNanos)
          + " next_frame_ms="
          + nanosToMillis(totalNextFrameNanos);
    }

    private static long nanosToMillis(long nanos) {
      return TimeUnit.NANOSECONDS.toMillis(nanos);
    }
  }

  private static final class SurfaceOutput implements AutoCloseable {
    private final ImageReader imageReader;
    private final Surface surface;
    private final HandlerThread callbackThread;
    private final ArrayBlockingQueue<Image> images =
        new ArrayBlockingQueue<>(MAXIMUM_ACQUIRED_IMAGES);
    private final AtomicReference<RuntimeException> callbackFailure = new AtomicReference<>();

    private SurfaceOutput(int width, int height) {
      imageReader =
          ImageReader.newInstance(
              width,
              height,
              ImageFormat.YUV_420_888,
              MAXIMUM_ACQUIRED_IMAGES);
      surface = imageReader.getSurface();
      callbackThread = new HandlerThread("pose-replay-decoder-output");
      callbackThread.start();
      imageReader.setOnImageAvailableListener(
          reader -> {
            try {
              Image image = reader.acquireNextImage();
              if (image != null && !images.offer(image)) {
                image.close();
                callbackFailure.compareAndSet(
                    null, new IllegalStateException("replay decoder image queue overflow"));
              }
            } catch (RuntimeException failure) {
              callbackFailure.compareAndSet(null, failure);
            }
          },
          new Handler(callbackThread.getLooper()));
    }

    private Image awaitImage() throws IOException {
      RuntimeException failure = callbackFailure.get();
      if (failure != null) {
        throw new IOException("replay decoder image callback failed", failure);
      }
      try {
        Image image = images.poll(IMAGE_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS);
        if (image == null) {
          failure = callbackFailure.get();
          if (failure != null) {
            throw new IOException("replay decoder image callback failed", failure);
          }
          throw new IOException("timed out waiting for replay decoder image");
        }
        return image;
      } catch (InterruptedException interrupted) {
        Thread.currentThread().interrupt();
        throw new IOException("interrupted waiting for replay decoder image", interrupted);
      }
    }

    @Override
    public void close() {
      imageReader.setOnImageAvailableListener(null, null);
      Image image;
      while ((image = images.poll()) != null) {
        image.close();
      }
      imageReader.close();
      surface.release();
      callbackThread.quitSafely();
      try {
        callbackThread.join(1_000L);
      } catch (InterruptedException interrupted) {
        Thread.currentThread().interrupt();
      }
    }
  }

  private final String sourceId;
  private final MediaExtractor extractor;
  private final MediaCodec decoder;
  private final String decoderName;
  private final SurfaceOutput surfaceOutput;
  private final FiveHertzFrameGate frameGate = new FiveHertzFrameGate();
  private final MediaCodec.BufferInfo bufferInfo = new MediaCodec.BufferInfo();

  private long nextFrameCalls;
  private long outputBuffers;
  private long skippedOutputBuffers;
  private long selectedFrames;
  private long totalDequeueNanos;
  private long totalImageWaitNanos;
  private long totalConversionNanos;
  private long totalNextFrameNanos;
  private boolean inputEnded;
  private boolean outputEnded;
  private boolean closed;

  private MediaCodecReplayFrameSource(
      String sourceId,
      MediaExtractor extractor,
      MediaCodec decoder,
      SurfaceOutput surfaceOutput) {
    this.sourceId = sourceId;
    this.extractor = extractor;
    this.decoder = decoder;
    decoderName = decoder.getName();
    this.surfaceOutput = surfaceOutput;
  }

  public static MediaCodecReplayFrameSource open(File appPrivateRoot, File clip)
      throws IOException {
    File root = Objects.requireNonNull(appPrivateRoot, "appPrivateRoot").getCanonicalFile();
    File canonicalClip = Objects.requireNonNull(clip, "clip").getCanonicalFile();
    if (!root.isDirectory()) {
      throw new IOException("appPrivateRoot must be a directory");
    }
    if (!isWithin(root, canonicalClip) || !canonicalClip.isFile()) {
      throw new IOException("clip must be a regular file within appPrivateRoot");
    }
    if (canonicalClip.length() <= 0 || canonicalClip.length() > MAX_CLIP_BYTES) {
      throw new IOException("clip size is outside replay bounds");
    }

    MediaExtractor extractor = new MediaExtractor();
    MediaCodec decoder = null;
    SurfaceOutput surfaceOutput = null;
    try {
      extractor.setDataSource(canonicalClip.getAbsolutePath());
      MediaFormat videoFormat = selectVideoTrack(extractor);
      long durationUs = videoFormat.getLong(MediaFormat.KEY_DURATION);
      if (durationUs <= 0 || durationUs > MAX_DURATION_US) {
        throw new IOException("clip duration is outside replay bounds");
      }
      // Software replay consumes output immediately rather than presenting it in real time. Some
      // Pixel decoder implementations otherwise pace buffer production near the source frame
      // rate even without a render Surface, making a short deterministic replay take wall-clock
      // clip duration. Declare a high offline operating rate and low-latency priority explicitly.
      videoFormat.setFloat(MediaFormat.KEY_OPERATING_RATE, 240.0f);
      videoFormat.setInteger(MediaFormat.KEY_PRIORITY, 0);
      if (android.os.Build.VERSION.SDK_INT >= 30) {
        videoFormat.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
      }
      try {
        surfaceOutput =
            new SurfaceOutput(
                videoFormat.getInteger(MediaFormat.KEY_WIDTH),
                videoFormat.getInteger(MediaFormat.KEY_HEIGHT));
        decoder = MediaCodec.createDecoderByType(videoFormat.getString(MediaFormat.KEY_MIME));
        decoder.configure(videoFormat, surfaceOutput.surface, null, 0);
        decoder.start();
      } catch (RuntimeException unsupportedSurfaceOutput) {
        if (decoder != null) {
          decoder.release();
          decoder = null;
        }
        if (surfaceOutput != null) {
          surfaceOutput.close();
          surfaceOutput = null;
        }
        // Preserve the proven flexible-YUV path as an initialization fallback. It is slower on
        // high-frame-rate clips but remains preferable to rejecting replay on a codec that cannot
        // target a CPU-readable ImageReader surface.
        videoFormat.setInteger(
            MediaFormat.KEY_COLOR_FORMAT,
            MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
        decoder = MediaCodec.createDecoderByType(videoFormat.getString(MediaFormat.KEY_MIME));
        decoder.configure(videoFormat, null, null, 0);
        decoder.start();
      }
      return new MediaCodecReplayFrameSource(
          canonicalClip.getName(), extractor, decoder, surfaceOutput);
    } catch (IOException | RuntimeException exception) {
      if (decoder != null) {
        decoder.release();
      }
      if (surfaceOutput != null) {
        surfaceOutput.close();
      }
      extractor.release();
      if (exception instanceof IOException ioException) {
        throw ioException;
      }
      throw new IOException("failed to initialize replay decoder", exception);
    }
  }

  @Override
  public String sourceId() {
    return sourceId;
  }

  @Override
  public RgbFrame nextFrame() throws IOException {
    ensureOpen();
    if (outputEnded) {
      return null;
    }
    long nextFrameStartedNanos = SystemClock.elapsedRealtimeNanos();
    ++nextFrameCalls;
    try {
      while (!outputEnded) {
        feedDecoder();
        long dequeueStartedNanos = SystemClock.elapsedRealtimeNanos();
        int outputIndex = decoder.dequeueOutputBuffer(bufferInfo, CODEC_TIMEOUT_US);
        totalDequeueNanos =
            saturatedAdd(
                totalDequeueNanos,
                elapsedSince(dequeueStartedNanos));
        if (outputIndex == MediaCodec.INFO_TRY_AGAIN_LATER
            || outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED
            || outputIndex == MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED) {
          continue;
        }
        if (outputIndex < 0) {
          continue;
        }
        ++outputBuffers;

        boolean endOfStream =
            (bufferInfo.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
        RgbFrame selected = null;
        Image image = null;
        boolean released = false;
        try {
          if (bufferInfo.size > 0) {
            long timestampNs = Math.max(0, bufferInfo.presentationTimeUs) * 1_000L;
            if (frameGate.accept(timestampNs)) {
              // Surface mode lets the hardware decoder discard the intervening 240 fps frames
              // without materializing a CPU-readable flexible-YUV buffer. Render only the exact
              // 5 Hz selection, then keep using the established RGB inference path.
              if (surfaceOutput != null) {
                decoder.releaseOutputBuffer(outputIndex, true);
                released = true;
                long imageWaitStartedNanos = SystemClock.elapsedRealtimeNanos();
                image = surfaceOutput.awaitImage();
                totalImageWaitNanos =
                    saturatedAdd(totalImageWaitNanos, elapsedSince(imageWaitStartedNanos));
              } else {
                image = decoder.getOutputImage(outputIndex);
                if (image == null) {
                  throw new IOException("decoder did not expose a flexible YUV image");
                }
              }
              long conversionStartedNanos = SystemClock.elapsedRealtimeNanos();
              selected = convertYuvImage(image, timestampNs);
              totalConversionNanos =
                  saturatedAdd(totalConversionNanos, elapsedSince(conversionStartedNanos));
              ++selectedFrames;
            } else {
              ++skippedOutputBuffers;
            }
          }
        } finally {
          if (image != null) {
            image.close();
          }
          if (!released) {
            decoder.releaseOutputBuffer(outputIndex, false);
          }
        }
        outputEnded = endOfStream;
        if (selected != null) {
          return selected;
        }
      }
      return null;
    } catch (IllegalStateException exception) {
      throw new IOException("replay decoder failed", exception);
    } finally {
      totalNextFrameNanos =
          saturatedAdd(totalNextFrameNanos, elapsedSince(nextFrameStartedNanos));
    }
  }

  public PerformanceSnapshot performanceSnapshot() {
    return new PerformanceSnapshot(
        decoderName,
        surfaceOutput == null ? "flexible_yuv_buffer" : "image_reader_surface",
        nextFrameCalls,
        outputBuffers,
        skippedOutputBuffers,
        selectedFrames,
        totalDequeueNanos,
        totalImageWaitNanos,
        totalConversionNanos,
        totalNextFrameNanos);
  }

  @Override
  public void close() {
    if (closed) {
      return;
    }
    closed = true;
    try {
      decoder.stop();
    } catch (IllegalStateException ignored) {
      // release() is still required after a codec failure.
    }
    decoder.release();
    extractor.release();
    if (surfaceOutput != null) {
      surfaceOutput.close();
    }
    outputEnded = true;
  }

  private void feedDecoder() {
    if (inputEnded) {
      return;
    }
    int inputIndex = decoder.dequeueInputBuffer(CODEC_TIMEOUT_US);
    if (inputIndex < 0) {
      return;
    }
    ByteBuffer inputBuffer = decoder.getInputBuffer(inputIndex);
    if (inputBuffer == null) {
      throw new IllegalStateException("decoder input buffer unavailable");
    }
    int sampleSize = extractor.readSampleData(inputBuffer, 0);
    if (sampleSize < 0) {
      decoder.queueInputBuffer(inputIndex, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
      inputEnded = true;
      return;
    }
    long sampleTimeUs = extractor.getSampleTime();
    decoder.queueInputBuffer(inputIndex, 0, sampleSize, sampleTimeUs, extractor.getSampleFlags());
    extractor.advance();
  }

  private static MediaFormat selectVideoTrack(MediaExtractor extractor) throws IOException {
    for (int track = 0; track < extractor.getTrackCount(); track++) {
      MediaFormat format = extractor.getTrackFormat(track);
      String mime = format.getString(MediaFormat.KEY_MIME);
      if (mime != null && mime.startsWith("video/")) {
        extractor.selectTrack(track);
        return format;
      }
    }
    throw new IOException("clip has no video track");
  }

  private static RgbFrame convertYuvImage(Image image, long timestampNs) throws IOException {
    if (image.getFormat() != ImageFormat.YUV_420_888) {
      throw new IOException("decoder output is not YUV_420_888");
    }
    Rect crop = image.getCropRect();
    int width = crop.width();
    int height = crop.height();
    Image.Plane[] planes = image.getPlanes();
    if (planes.length != 3) {
      throw new IOException("decoder output does not have three YUV planes");
    }
    return StrideAwareYuv420ToRgb.convert(
        width,
        height,
        timestampNs,
        plane(planes[0], crop.left, crop.top),
        plane(planes[1], crop.left / 2, crop.top / 2),
        plane(planes[2], crop.left / 2, crop.top / 2));
  }

  private static StrideAwareYuv420ToRgb.Plane plane(
      Image.Plane plane, int left, int top) {
    ByteBuffer buffer = plane.getBuffer().duplicate();
    int offset =
        buffer.position() + top * plane.getRowStride() + left * plane.getPixelStride();
    return new StrideAwareYuv420ToRgb.Plane(
        buffer, offset, plane.getRowStride(), plane.getPixelStride());
  }

  private static boolean isWithin(File root, File candidate) {
    for (File cursor = candidate; cursor != null; cursor = cursor.getParentFile()) {
      if (root.equals(cursor)) {
        return true;
      }
    }
    return false;
  }

  private static long elapsedSince(long startedNanos) {
    return Math.max(0, SystemClock.elapsedRealtimeNanos() - startedNanos);
  }

  private static long saturatedAdd(long left, long right) {
    if (right > Long.MAX_VALUE - left) {
      return Long.MAX_VALUE;
    }
    return left + right;
  }

  private void ensureOpen() {
    if (closed) {
      throw new IllegalStateException("replay source is closed");
    }
  }
}
