package com.agoessling.swingcapture.pose;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/** Host CLI for replaying an entire labeled field session with production camera blindness. */
public final class PoseFieldSessionReplayCli {
  private static final long MILLIS_TO_NANOS = 1_000_000L;
  private static final Set<String> ALLOWED_OPTIONS =
      Set.of(
          "observations",
          "impacts-ms",
          "takeaways-ms",
          "video-startup-ms",
          "audio-ready-ms",
          "post-impact-ms",
          "no-impact-completion-ms",
          "restart-ms",
          "minimum-person",
          "minimum-address",
          "maximum-motion",
          "qualification-ms",
          "active-ms",
          "clear-ms",
          "cooldown-ms",
          "thermal-cap-ms",
          "output");

  private PoseFieldSessionReplayCli() {}

  public static void main(String[] arguments) throws IOException {
    Map<String, String> options = parseOptions(arguments);
    String encoded = run(options);
    if (options.containsKey("output")) {
      Files.writeString(Path.of(options.get("output")), encoded + "\n", StandardCharsets.UTF_8);
    } else {
      System.out.println(encoded);
    }
  }

  static String run(Map<String, String> options) throws IOException {
    PoseTriggerController.Config defaults =
        PoseTriggerController.Config.defaultsForFiveFramesPerSecond();
    PoseTriggerController.Config controllerConfig =
        new PoseTriggerController.Config(
            doubleOption(options, "minimum-person", defaults.minimumPersonConfidence()),
            defaults.maximumClearPersonConfidence(),
            doubleOption(options, "minimum-address", defaults.minimumAddressConfidence()),
            doubleOption(options, "maximum-motion", defaults.maximumMotionMagnitude()),
            nanosOption(options, "qualification-ms", defaults.minimumQualificationNs()),
            defaults.maximumObservationGapNs(),
            defaults.qualificationDropoutGraceNs(),
            nanosOption(options, "active-ms", defaults.maximumArmedDurationNs()),
            nanosOption(options, "clear-ms", defaults.clearDurationNs()),
            nanosOption(options, "cooldown-ms", defaults.cooldownNs()),
            nanosOption(options, "thermal-cap-ms", defaults.thermalHardCapNs()));
    PoseFieldSessionReplay.Config replayConfig =
        new PoseFieldSessionReplay.Config(
            controllerConfig,
            nanosOption(options, "video-startup-ms", 800_000_000L),
            nanosOption(options, "audio-ready-ms", 2_450_000_000L),
            nanosOption(options, "post-impact-ms", 1_000_000_000L),
            nanosOption(options, "no-impact-completion-ms", 1_000_000_000L),
            nanosOption(options, "restart-ms", 800_000_000L));
    List<Long> impacts = parseMillisList(required(options, "impacts-ms"), "impacts-ms");
    List<Long> takeaways = parseMillisList(required(options, "takeaways-ms"), "takeaways-ms");
    if (impacts.size() != takeaways.size()) {
      throw new IllegalArgumentException("impacts-ms and takeaways-ms must have equal lengths");
    }
    ArrayList<PoseFieldSessionReplay.Target> targets = new ArrayList<>();
    for (int index = 0; index < impacts.size(); ++index) {
      targets.add(
          new PoseFieldSessionReplay.Target(
              String.format(Locale.ROOT, "S%02d", index + 1), impacts.get(index), takeaways.get(index)));
    }
    String observationsCsv =
        Files.readString(Path.of(required(options, "observations")), StandardCharsets.UTF_8);
    PoseFieldSessionReplay.Result result =
        PoseFieldSessionReplay.evaluate(
            PoseObservationCsv.parse(observationsCsv), targets, replayConfig);
    return toJson(replayConfig, result);
  }

  private static Map<String, String> parseOptions(String[] arguments) {
    HashMap<String, String> options = new HashMap<>();
    for (String argument : arguments) {
      if (!argument.startsWith("--") || !argument.contains("=")) {
        throw new IllegalArgumentException("options must use --name=value syntax");
      }
      int separator = argument.indexOf('=');
      String name = argument.substring(2, separator);
      String value = argument.substring(separator + 1);
      if (!ALLOWED_OPTIONS.contains(name)) {
        throw new IllegalArgumentException("unknown option: " + name);
      }
      if (value.isEmpty()) {
        throw new IllegalArgumentException("option cannot be empty: " + name);
      }
      if (options.put(name, value) != null) {
        throw new IllegalArgumentException("duplicate option: " + name);
      }
    }
    return Map.copyOf(options);
  }

  private static String required(Map<String, String> options, String name) {
    String value = options.get(name);
    if (value == null) {
      throw new IllegalArgumentException("missing required option: " + name);
    }
    return value;
  }

  private static long nanosOption(Map<String, String> options, String name, long defaultNs) {
    String encoded = options.get(name);
    return encoded == null ? defaultNs : millisToNanos(parseDouble(encoded, name));
  }

  private static double doubleOption(
      Map<String, String> options, String name, double defaultValue) {
    String encoded = options.get(name);
    return encoded == null ? defaultValue : parseDouble(encoded, name);
  }

  private static List<Long> parseMillisList(String encoded, String name) {
    ArrayList<Long> values = new ArrayList<>();
    for (String field : encoded.split(",", -1)) {
      values.add(millisToNanos(parseDouble(field, name)));
    }
    return List.copyOf(values);
  }

  private static double parseDouble(String encoded, String name) {
    try {
      double value = Double.parseDouble(encoded);
      if (!Double.isFinite(value) || value < 0) {
        throw new IllegalArgumentException(name + " must be finite and nonnegative");
      }
      return value;
    } catch (NumberFormatException exception) {
      throw new IllegalArgumentException("invalid number for " + name, exception);
    }
  }

  private static long millisToNanos(double milliseconds) {
    double nanoseconds = milliseconds * MILLIS_TO_NANOS;
    if (nanoseconds > Long.MAX_VALUE) {
      throw new IllegalArgumentException("millisecond value is too large");
    }
    return Math.round(nanoseconds);
  }

  private static String toJson(
      PoseFieldSessionReplay.Config config, PoseFieldSessionReplay.Result result) {
    StringBuilder json =
        new StringBuilder("{\"schema_version\":1,\"video_startup_budget_ns\":\"")
            .append(config.videoStartupBudgetNs())
            .append("\",\"audio_trigger_ready_budget_ns\":\"")
            .append(config.audioTriggerStartupBudgetNs())
            .append("\",\"captured_target_count\":")
            .append(result.capturedTargetCount())
            .append(",\"target_count\":")
            .append(result.targets().size())
            .append(",\"attempt_count\":")
            .append(result.attempts().size())
            .append(",\"total_high_speed_ns\":\"")
            .append(result.totalHighSpeedNs())
            .append("\",\"skipped_pose_observations\":")
            .append(result.skippedPoseObservations())
            .append(",\"attempts\":[");
    for (int index = 0; index < result.attempts().size(); ++index) {
      if (index > 0) {
        json.append(',');
      }
      PoseFieldSessionReplay.Attempt attempt = result.attempts().get(index);
      json.append("{\"arm_ns\":\"")
          .append(attempt.armNs())
          .append("\",\"video_ready_ns\":\"")
          .append(attempt.videoReadyNs())
          .append("\",\"audio_trigger_ready_ns\":\"")
          .append(attempt.audioTriggerReadyNs())
          .append("\",\"completion_ns\":\"")
          .append(attempt.completionNs())
          .append("\",\"outcome\":\"")
          .append(attempt.outcome().name().toLowerCase(Locale.ROOT))
          .append("\",\"captured_target_id\":\"")
          .append(attempt.capturedTargetId())
          .append("\"}");
    }
    json.append("],\"targets\":[");
    for (int index = 0; index < result.targets().size(); ++index) {
      if (index > 0) {
        json.append(',');
      }
      PoseFieldSessionReplay.TargetResult target = result.targets().get(index);
      json.append("{\"id\":\"")
          .append(target.target().id())
          .append("\",\"impact_ns\":\"")
          .append(target.target().impactNs())
          .append("\",\"outcome\":\"")
          .append(target.outcome().name().toLowerCase(Locale.ROOT))
          .append("\",\"arm_ns\":")
          .append(target.captured() ? "\"" + target.armNs() + "\"" : "null")
          .append(",\"video_lead_before_takeaway_ns\":")
          .append(
              target.armNs() >= 0
                  ? "\"" + target.videoLeadBeforeTakeawayNs() + "\""
                  : "null")
          .append(",\"audio_ready_lead_before_impact_ns\":")
          .append(
              target.armNs() >= 0
                  ? "\"" + target.audioReadyLeadBeforeImpactNs() + "\""
                  : "null")
          .append('}');
    }
    return json.append("]}").toString();
  }
}
