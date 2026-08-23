package com.agoessling.swingcapture;

import com.agoessling.swingcapture.audio.ImpactDetector;
import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

/** Host replay boundary for the exact detector and per-device policy used by Android capture. */
public final class ProductionAudioDetectorReplayCli {
  private static final int BLOCK_FRAMES = 4096;

  private record Candidate(long strikeMs, long decisionMs) {}

  private record Attempt(String id, long armMs, long endMs) {}

  private ProductionAudioDetectorReplayCli() {}

  public static void main(String[] args) throws Exception {
    if (args.length == 3 && args[0].equals("describe")) {
      System.out.println(configJson(args[1], args[2]));
      return;
    }
    if (args.length == 4 && args[0].equals("predict")) {
      predict(Path.of(args[1]), args[2], args[3]);
      return;
    }
    throw new IllegalArgumentException(
        "usage: describe MANUFACTURER MODEL | predict WAV MANUFACTURER MODEL");
  }

  private static void predict(Path wavPath, String manufacturer, String model) throws Exception {
    short[] pcm = readMonoPcm16Wav(wavPath);
    ImpactDetector.Config config = DeviceAudioDetectorPolicy.forDevice(manufacturer, model);
    List<Attempt> attempts = readAttempts();
    StringBuilder result = new StringBuilder();
    result.append("{\"config\":").append(configJson(manufacturer, model));
    result.append(",\"continuous_candidates\":");
    appendCandidates(result, replay(pcm, 0, pcm.length, config));
    result.append(",\"armed_replays\":[");
    for (int index = 0; index < attempts.size(); ++index) {
      Attempt attempt = attempts.get(index);
      if (index != 0) {
        result.append(',');
      }
      int firstFrame = millisecondsToFrame(attempt.armMs(), pcm.length);
      int endFrame = millisecondsToFrame(attempt.endMs(), pcm.length);
      result.append("{\"attempt_id\":\"").append(escapeJson(attempt.id())).append("\"");
      result.append(",\"arm_ms\":").append(attempt.armMs());
      result.append(",\"evaluation_end_ms\":").append(attempt.endMs());
      result.append(",\"candidates\":");
      appendCandidates(result, replay(pcm, firstFrame, endFrame, config));
      result.append('}');
    }
    result.append("]}");
    System.out.println(result);
  }

  private static List<Attempt> readAttempts() throws IOException {
    List<Attempt> result = new ArrayList<>();
    try (BufferedReader input =
        new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8))) {
      for (String line = input.readLine(); line != null; line = input.readLine()) {
        if (line.isEmpty()) {
          continue;
        }
        String[] fields = line.split("\\t", -1);
        if (fields.length != 3 || fields[0].isEmpty()) {
          throw new IllegalArgumentException("attempt input must be ID<TAB>ARM_MS<TAB>END_MS");
        }
        long armMs = Long.parseLong(fields[1]);
        long endMs = Long.parseLong(fields[2]);
        if (armMs < 0 || endMs < armMs) {
          throw new IllegalArgumentException("attempt interval is invalid");
        }
        result.add(new Attempt(fields[0], armMs, endMs));
      }
    }
    return result;
  }

  private static List<Candidate> replay(
      short[] pcm, int firstFrame, int endFrame, ImpactDetector.Config config) {
    ImpactDetector detector = new ImpactDetector(config);
    List<Candidate> candidates = new ArrayList<>();
    for (int blockStart = firstFrame; blockStart < endFrame; blockStart += BLOCK_FRAMES) {
      int length = Math.min(BLOCK_FRAMES, endFrame - blockStart);
      detector.processBlock(
          pcm,
          blockStart,
          length,
          blockStart,
          impact ->
              candidates.add(
                  new Candidate(
                      frameToMilliseconds(impact.strikeFramePosition()),
                      frameToMilliseconds(impact.confirmationFramePosition()))));
    }
    return candidates;
  }

  private static int millisecondsToFrame(long milliseconds, int maximum) {
    long frame = Math.multiplyExact(milliseconds, ImpactDetector.SAMPLE_RATE_HZ) / 1000;
    return (int) Math.min(frame, maximum);
  }

  private static long frameToMilliseconds(long frame) {
    return Math.multiplyExact(frame, 1000) / ImpactDetector.SAMPLE_RATE_HZ;
  }

  private static void appendCandidates(StringBuilder output, List<Candidate> candidates) {
    output.append('[');
    for (int index = 0; index < candidates.size(); ++index) {
      if (index != 0) {
        output.append(',');
      }
      Candidate candidate = candidates.get(index);
      output
          .append("{\"strike_ms\":")
          .append(candidate.strikeMs())
          .append(",\"decision_ms\":")
          .append(candidate.decisionMs())
          .append('}');
    }
    output.append(']');
  }

  private static String configJson(String manufacturer, String model) {
    ImpactDetector.Config config = DeviceAudioDetectorPolicy.forDevice(manufacturer, model);
    return new StringBuilder()
        .append("{\"manufacturer\":\"")
        .append(escapeJson(manufacturer))
        .append("\",\"model\":\"")
        .append(escapeJson(model))
        .append("\",\"threshold_multiplier\":")
        .append(config.thresholdMultiplier())
        .append(",\"minimum_peak_amplitude\":")
        .append(config.minimumPeakAmplitude())
        .append(",\"initial_noise_floor\":")
        .append(config.initialNoiseFloor())
        .append(",\"noise_update_clip_multiplier\":")
        .append(config.noiseUpdateClipMultiplier())
        .append(",\"noise_floor_time_constant_seconds\":")
        .append(config.noiseFloorTimeConstantSeconds())
        .append(",\"peak_confirmation_frames\":")
        .append(config.peakConfirmationFrames())
        .append(",\"cooldown_frames\":")
        .append(config.cooldownFrames())
        .append('}')
        .toString();
  }

  private static short[] readMonoPcm16Wav(Path path) throws IOException {
    byte[] bytes = Files.readAllBytes(path);
    if (bytes.length < 12
        || !asciiEquals(bytes, 0, "RIFF")
        || !asciiEquals(bytes, 8, "WAVE")) {
      throw new IllegalArgumentException("input is not a RIFF/WAVE file");
    }
    ByteBuffer input = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
    int cursor = 12;
    boolean formatSeen = false;
    int dataOffset = -1;
    int dataLength = -1;
    while (cursor <= bytes.length - 8) {
      long rawLength = Integer.toUnsignedLong(input.getInt(cursor + 4));
      if (rawLength > Integer.MAX_VALUE || cursor + 8L + rawLength > bytes.length) {
        throw new IllegalArgumentException("WAV chunk extends past end of file");
      }
      int length = (int) rawLength;
      if (asciiEquals(bytes, cursor, "fmt ")) {
        if (length < 16) {
          throw new IllegalArgumentException("WAV fmt chunk is truncated");
        }
        int format = Short.toUnsignedInt(input.getShort(cursor + 8));
        int channels = Short.toUnsignedInt(input.getShort(cursor + 10));
        int sampleRate = input.getInt(cursor + 12);
        int blockAlign = Short.toUnsignedInt(input.getShort(cursor + 20));
        int bitsPerSample = Short.toUnsignedInt(input.getShort(cursor + 22));
        if (format != 1
            || channels != 1
            || sampleRate != ImpactDetector.SAMPLE_RATE_HZ
            || blockAlign != 2
            || bitsPerSample != 16) {
          throw new IllegalArgumentException("WAV must be mono 48 kHz signed PCM16");
        }
        formatSeen = true;
      } else if (asciiEquals(bytes, cursor, "data")) {
        dataOffset = cursor + 8;
        dataLength = length;
      }
      cursor += 8 + length + (length & 1);
    }
    if (!formatSeen || dataOffset < 0 || (dataLength & 1) != 0) {
      throw new IllegalArgumentException("WAV is missing a valid fmt or data chunk");
    }
    short[] result = new short[dataLength / 2];
    for (int index = 0; index < result.length; ++index) {
      result[index] = input.getShort(dataOffset + index * 2);
    }
    return result;
  }

  private static boolean asciiEquals(byte[] bytes, int offset, String expected) {
    if (offset < 0 || offset + expected.length() > bytes.length) {
      return false;
    }
    for (int index = 0; index < expected.length(); ++index) {
      if (bytes[offset + index] != (byte) expected.charAt(index)) {
        return false;
      }
    }
    return true;
  }

  private static String escapeJson(String value) {
    return value.replace("\\", "\\\\").replace("\"", "\\\"");
  }
}
