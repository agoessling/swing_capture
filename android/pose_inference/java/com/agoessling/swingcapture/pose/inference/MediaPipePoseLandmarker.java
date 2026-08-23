package com.agoessling.swingcapture.pose.inference;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.ImageFormat;
import android.media.Image;
import com.agoessling.swingcapture.pose.NormalizedPoseLandmark;
import com.agoessling.swingcapture.pose.PoseJoint;
import com.agoessling.swingcapture.pose.PoseLandmarkFrame;
import com.google.mediapipe.framework.image.BitmapImageBuilder;
import com.google.mediapipe.framework.image.ByteBufferImageBuilder;
import com.google.mediapipe.framework.image.MPImage;
import com.google.mediapipe.tasks.components.containers.NormalizedLandmark;
import com.google.mediapipe.tasks.core.BaseOptions;
import com.google.mediapipe.tasks.core.Delegate;
import com.google.mediapipe.tasks.vision.core.ImageProcessingOptions;
import com.google.mediapipe.tasks.vision.core.RunningMode;
import com.google.mediapipe.tasks.vision.poselandmarker.PoseLandmarker;
import com.google.mediapipe.tasks.vision.poselandmarker.PoseLandmarkerResult;
import java.nio.ByteBuffer;
import java.util.EnumMap;
import java.util.List;
import java.util.Objects;

/** Synchronous MediaPipe Pose Landmarker wrapper with explicit model and delegate policy. */
public final class MediaPipePoseLandmarker implements PoseFrameInference {
  public static final String MODEL_ASSET_PATH = PoseModelVariant.LITE.assetPath();

  private static final PoseJoint[] REQUIRED_JOINTS = {
    PoseJoint.LEFT_SHOULDER,
    PoseJoint.RIGHT_SHOULDER,
    PoseJoint.LEFT_ELBOW,
    PoseJoint.RIGHT_ELBOW,
    PoseJoint.LEFT_WRIST,
    PoseJoint.RIGHT_WRIST,
    PoseJoint.LEFT_HIP,
    PoseJoint.RIGHT_HIP,
    PoseJoint.LEFT_KNEE,
    PoseJoint.RIGHT_KNEE,
    PoseJoint.LEFT_ANKLE,
    PoseJoint.RIGHT_ANKLE
  };
  private static final int[] MEDIAPIPE_INDICES = {11, 12, 13, 14, 15, 16, 23, 24, 25, 26, 27, 28};

  private final PoseLandmarker landmarker;
  private final PoseInferenceDelegate actualDelegate;
  private final PoseModelVariant modelVariant;
  private final ImageProcessingOptions[] processingOptions = new ImageProcessingOptions[4];
  private ByteBuffer cameraRgbBuffer;
  private int cameraBufferWidth;
  private int cameraBufferHeight;
  private long lastTimestampMs = -1;
  private boolean closed;

  private MediaPipePoseLandmarker(
      PoseLandmarker landmarker,
      PoseInferenceDelegate actualDelegate,
      PoseModelVariant modelVariant) {
    this.landmarker = Objects.requireNonNull(landmarker, "landmarker");
    this.actualDelegate = Objects.requireNonNull(actualDelegate, "actualDelegate");
    this.modelVariant = Objects.requireNonNull(modelVariant, "modelVariant");
    for (int quarterTurn = 0; quarterTurn < processingOptions.length; ++quarterTurn) {
      processingOptions[quarterTurn] =
          ImageProcessingOptions.builder().setRotationDegrees(quarterTurn * 90).build();
    }
  }

  public static MediaPipePoseLandmarker open(
      Context context, PoseInferenceDelegatePolicy policy) {
    return open(context, policy, PoseModelVariant.productionDefault());
  }

  public static MediaPipePoseLandmarker open(
      Context context, PoseInferenceDelegatePolicy policy, PoseModelVariant modelVariant) {
    Objects.requireNonNull(context, "context");
    Objects.requireNonNull(policy, "policy");
    Objects.requireNonNull(modelVariant, "modelVariant");
    return switch (policy) {
      case CPU_ONLY -> create(context, PoseInferenceDelegate.CPU, modelVariant);
      case GPU_REQUIRED -> create(context, PoseInferenceDelegate.GPU, modelVariant);
      case NPU_REQUIRED -> create(context, PoseInferenceDelegate.NPU, modelVariant);
      case GPU_PREFERRED -> {
        try {
          yield create(context, PoseInferenceDelegate.GPU, modelVariant);
        } catch (RuntimeException gpuFailure) {
          yield create(context, PoseInferenceDelegate.CPU, modelVariant);
        }
      }
      case NPU_PREFERRED -> {
        try {
          yield create(context, PoseInferenceDelegate.NPU, modelVariant);
        } catch (RuntimeException npuFailure) {
          try {
            yield create(context, PoseInferenceDelegate.GPU, modelVariant);
          } catch (RuntimeException gpuFailure) {
            yield create(context, PoseInferenceDelegate.CPU, modelVariant);
          }
        }
      }
    };
  }

  public PoseModelVariant modelVariant() {
    return modelVariant;
  }

  @Override
  public PoseInferenceDelegate actualDelegate() {
    return actualDelegate;
  }

  /** Converts replay/test ARGB data at the MediaPipe boundary. */
  @Override
  public synchronized PoseLandmarkFrame infer(RgbFrame frame) {
    Objects.requireNonNull(frame, "frame");
    ensureOpen();
    long timestampMs = validateTimestamp(frame.timestampNs());
    Bitmap bitmap = Bitmap.createBitmap(frame.width(), frame.height(), Bitmap.Config.ARGB_8888);
    bitmap.setPixels(frame.argb(), 0, frame.width(), 0, 0, frame.width(), frame.height());
    try (MPImage mpImage = new BitmapImageBuilder(bitmap).build()) {
      return convert(landmarker.detectForVideo(mpImage, timestampMs), frame.timestampNs());
    } finally {
      bitmap.recycle();
    }
  }

  /** Converts a Camera2 YUV image into a reusable row-aligned RGB buffer for MediaPipe. */
  public synchronized PoseLandmarkFrame infer(
      Image image, long timestampNs, int rotationDegrees) {
    Objects.requireNonNull(image, "image");
    ensureOpen();
    requireSupportedRotation(rotationDegrees);
    if (image.getFormat() != ImageFormat.YUV_420_888 || image.getPlanes().length != 3) {
      throw new IllegalArgumentException("camera inference requires YUV_420_888 with three planes");
    }
    long timestampMs = validateTimestamp(timestampNs);
    ByteBuffer rgb = cameraRgbBuffer(image.getWidth(), image.getHeight());
    Image.Plane[] planes = image.getPlanes();
    StrideAwareYuv420ToRgb.convertToRgb(
        image.getWidth(),
        image.getHeight(),
        plane(planes[0]),
        plane(planes[1]),
        plane(planes[2]),
        rgb);
    try (MPImage mpImage =
        new ByteBufferImageBuilder(
                rgb, image.getWidth(), image.getHeight(), MPImage.IMAGE_FORMAT_RGB)
            .build()) {
      return convert(
          landmarker.detectForVideo(
              mpImage, processingOptions[rotationDegrees / 90], timestampMs),
          timestampNs);
    }
  }

  @Override
  public synchronized void close() {
    if (!closed) {
      closed = true;
      landmarker.close();
      cameraRgbBuffer = null;
    }
  }

  private static MediaPipePoseLandmarker create(
      Context context, PoseInferenceDelegate delegate, PoseModelVariant modelVariant) {
    Delegate mediaPipeDelegate =
        switch (delegate) {
          case CPU -> Delegate.CPU;
          case GPU -> Delegate.GPU;
          case NPU -> Delegate.NPU;
        };
    BaseOptions baseOptions =
        BaseOptions.builder()
            .setModelAssetPath(modelVariant.assetPath())
            .setDelegate(mediaPipeDelegate)
            .build();
    PoseLandmarker.PoseLandmarkerOptions options =
        PoseLandmarker.PoseLandmarkerOptions.builder()
            .setBaseOptions(baseOptions)
            .setRunningMode(RunningMode.VIDEO)
            .setNumPoses(1)
            .setMinPoseDetectionConfidence(0.5f)
            .setMinPosePresenceConfidence(0.5f)
            .setMinTrackingConfidence(0.5f)
            .setOutputSegmentationMasks(false)
            .build();
    return new MediaPipePoseLandmarker(
        PoseLandmarker.createFromOptions(context.getApplicationContext(), options),
        delegate,
        modelVariant);
  }

  private static PoseLandmarkFrame convert(PoseLandmarkerResult result, long timestampNs) {
    if (result.landmarks().isEmpty()) {
      return new PoseLandmarkFrame(timestampNs, 0.0, new EnumMap<>(PoseJoint.class));
    }
    List<NormalizedLandmark> source = result.landmarks().get(0);
    EnumMap<PoseJoint, NormalizedPoseLandmark> landmarks = new EnumMap<>(PoseJoint.class);
    double confidenceSum = 0.0;
    for (int index = 0; index < REQUIRED_JOINTS.length; index++) {
      NormalizedLandmark landmark = source.get(MEDIAPIPE_INDICES[index]);
      double visibility = clampUnit(landmark.visibility().orElse(0.0f));
      double presence = clampUnit(landmark.presence().orElse((float) visibility));
      confidenceSum += presence;
      landmarks.put(
          REQUIRED_JOINTS[index],
          new NormalizedPoseLandmark(
              clampUnit(landmark.x()), clampUnit(landmark.y()), visibility));
    }
    return new PoseLandmarkFrame(
        timestampNs, confidenceSum / REQUIRED_JOINTS.length, landmarks);
  }

  private long validateTimestamp(long timestampNs) {
    if (timestampNs < 0) {
      throw new IllegalArgumentException("timestampNs cannot be negative");
    }
    long timestampMs = timestampNs / 1_000_000L;
    if (timestampMs <= lastTimestampMs) {
      throw new IllegalArgumentException("VIDEO timestamps must increase by at least one millisecond");
    }
    lastTimestampMs = timestampMs;
    return timestampMs;
  }

  private static void requireSupportedRotation(int rotationDegrees) {
    if (rotationDegrees != 0
        && rotationDegrees != 90
        && rotationDegrees != 180
        && rotationDegrees != 270) {
      throw new IllegalArgumentException("rotationDegrees must be 0, 90, 180, or 270");
    }
  }

  private ByteBuffer cameraRgbBuffer(int width, int height) {
    if (cameraRgbBuffer == null
        || cameraBufferWidth != width
        || cameraBufferHeight != height) {
      int byteCount = StrideAwareYuv420ToRgb.rgbBufferSize(width, height);
      cameraRgbBuffer = ByteBuffer.allocateDirect(byteCount);
      cameraBufferWidth = width;
      cameraBufferHeight = height;
    }
    return cameraRgbBuffer;
  }

  private static StrideAwareYuv420ToRgb.Plane plane(Image.Plane plane) {
    ByteBuffer buffer = plane.getBuffer().duplicate();
    return new StrideAwareYuv420ToRgb.Plane(
        buffer, buffer.position(), plane.getRowStride(), plane.getPixelStride());
  }

  private static double clampUnit(double value) {
    if (!Double.isFinite(value)) {
      return 0.0;
    }
    return Math.min(1.0, Math.max(0.0, value));
  }

  private void ensureOpen() {
    if (closed) {
      throw new IllegalStateException("pose landmarker is closed");
    }
  }
}
