package com.agoessling.swingcapture;

import android.Manifest;
import android.app.ActivityManager;
import android.content.Context;
import android.content.pm.ConfigurationInfo;
import android.content.pm.PackageManager;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Build;
import android.util.Range;
import android.util.Size;
import java.util.Arrays;

/** Android-framework adapter for the pure minimum-device admission policy. */
final class AndroidDeviceCapabilityProbe {
  private static final int HIGH_SPEED_FPS = 240;
  private static final int AUDIO_SAMPLE_RATE_HZ = 48_000;

  private AndroidDeviceCapabilityProbe() {}

  static DeviceCapabilityPolicy.HardwareSnapshot collect(Context context) throws Exception {
    Context application = context.getApplicationContext();
    boolean cameraPermission = granted(application, Manifest.permission.CAMERA);
    boolean audioPermission = granted(application, Manifest.permission.RECORD_AUDIO);
    CameraSupport cameras = cameraSupport(application);
    EncoderSupport encoders = encoderSupport();
    return new DeviceCapabilityPolicy.HardwareSnapshot(
        Build.VERSION.SDK_INT,
        cameraPermission,
        audioPermission,
        cameras.hd240(),
        cameras.fullHd240(),
        encoders.hd240(),
        encoders.fullHd240(),
        AudioRecord.getMinBufferSize(
                AUDIO_SAMPLE_RATE_HZ,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT)
            > 0,
        requiredOpenGlEsVersion(application),
        "");
  }

  static DeviceCapabilityPolicy.HardwareSnapshot collectOrUnavailable(Context context) {
    Context application = context.getApplicationContext();
    try {
      return collect(application);
    } catch (Exception failure) {
      return DeviceCapabilityPolicy.HardwareSnapshot.unavailable(
          Build.VERSION.SDK_INT,
          granted(application, Manifest.permission.CAMERA),
          granted(application, Manifest.permission.RECORD_AUDIO),
          requiredOpenGlEsVersion(application),
          failure.getClass().getSimpleName());
    }
  }

  private static boolean granted(Context context, String permission) {
    return context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
  }

  private static int requiredOpenGlEsVersion(Context context) {
    ActivityManager manager = context.getSystemService(ActivityManager.class);
    ConfigurationInfo configuration = manager == null ? null : manager.getDeviceConfigurationInfo();
    return configuration == null ? 0 : configuration.reqGlEsVersion;
  }

  private record CameraSupport(boolean hd240, boolean fullHd240) {}

  private static CameraSupport cameraSupport(Context context) throws Exception {
    CameraManager manager = context.getSystemService(CameraManager.class);
    if (manager == null) {
      return new CameraSupport(false, false);
    }
    boolean hd240 = false;
    boolean fullHd240 = false;
    for (String cameraId : manager.getCameraIdList()) {
      CameraCharacteristics camera = manager.getCameraCharacteristics(cameraId);
      Integer facing = camera.get(CameraCharacteristics.LENS_FACING);
      Integer timestampSource = camera.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE);
      Integer sensorOrientation = camera.get(CameraCharacteristics.SENSOR_ORIENTATION);
      StreamConfigurationMap streams =
          camera.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
      if (facing == null
          || facing != CameraCharacteristics.LENS_FACING_BACK
          || timestampSource == null
          || timestampSource != CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME
          || sensorOrientation == null
          || streams == null
          || !supportedOrientation(sensorOrientation)
          || !hasProductionStandby(streams)) {
        continue;
      }
      hd240 |= hasFixedHighSpeedProfile(streams, CaptureProfile.HD_240);
      fullHd240 |= hasFixedHighSpeedProfile(streams, CaptureProfile.FULL_HD_240);
    }
    return new CameraSupport(hd240, fullHd240);
  }

  private static boolean supportedOrientation(int sensorOrientation) {
    try {
      CameraImageRotation.fromSensorOrientation(sensorOrientation);
      return true;
    } catch (IllegalArgumentException unsupported) {
      return false;
    }
  }

  private static boolean hasProductionStandby(StreamConfigurationMap streams) {
    Size[] sizes = streams.getOutputSizes(ImageFormat.YUV_420_888);
    return sizes != null
        && Arrays.stream(sizes)
            .anyMatch(
                size ->
                    size.getWidth() >= DeviceCapabilityPolicy.POSE_INPUT_WIDTH
                        && size.getHeight() >= DeviceCapabilityPolicy.POSE_INPUT_HEIGHT
                        && Math.abs(size.getWidth() / (double) size.getHeight() - 16.0 / 9.0)
                            <= 0.03);
  }

  private static boolean hasFixedHighSpeedProfile(
      StreamConfigurationMap streams, CaptureProfile profile) {
    Size size = new Size(profile.width(), profile.height());
    if (!Arrays.asList(streams.getHighSpeedVideoSizes()).contains(size)) {
      return false;
    }
    Range<Integer>[] rates = streams.getHighSpeedVideoFpsRangesFor(size);
    return Arrays.stream(rates)
        .anyMatch(
            rate -> rate.getLower() == HIGH_SPEED_FPS && rate.getUpper() == HIGH_SPEED_FPS);
  }

  private record EncoderSupport(boolean hd240, boolean fullHd240) {}

  private static EncoderSupport encoderSupport() {
    boolean hd240 = false;
    boolean fullHd240 = false;
    for (MediaCodecInfo codec : new MediaCodecList(MediaCodecList.ALL_CODECS).getCodecInfos()) {
      if (!codec.isEncoder() || !codec.isHardwareAccelerated()) {
        continue;
      }
      for (String mime : codec.getSupportedTypes()) {
        if (!MediaFormat.MIMETYPE_VIDEO_AVC.equalsIgnoreCase(mime)) {
          continue;
        }
        MediaCodecInfo.VideoCapabilities video;
        try {
          video = codec.getCapabilitiesForType(mime).getVideoCapabilities();
        } catch (IllegalArgumentException unavailable) {
          continue;
        }
        hd240 |= supports(video, CaptureProfile.HD_240);
        fullHd240 |= supports(video, CaptureProfile.FULL_HD_240);
      }
    }
    return new EncoderSupport(hd240, fullHd240);
  }

  private static boolean supports(
      MediaCodecInfo.VideoCapabilities video, CaptureProfile profile) {
    try {
      return video.areSizeAndRateSupported(profile.width(), profile.height(), HIGH_SPEED_FPS);
    } catch (IllegalArgumentException unsupported) {
      return false;
    }
  }
}
