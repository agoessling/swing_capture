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
import java.util.OptionalLong;
import java.util.Set;

/** Host replay entry point for pose observations sampled from a recorded video. */
public final class PoseTriggerReplayCli {
  private static final long MILLIS_TO_NANOS = 1_000_000L;
  private static final Set<String> ALLOWED_OPTIONS =
      Set.of(
          "observations",
          "safe-arm-start-ms",
          "preferred-arm-ms",
          "takeaway-ms",
          "startup-budget-ms",
          "must-not-arm-ms");

  private PoseTriggerReplayCli() {}

  public static void main(String[] arguments) throws IOException {
    System.out.println(run(arguments));
  }

  static String run(String[] arguments) throws IOException {
    Map<String, String> options = parseOptions(arguments);
    String csv =
        Files.readString(Path.of(required(options, "observations")), StandardCharsets.UTF_8);
    List<PoseTriggerController.Observation> observations = PoseObservationCsv.parse(csv);
    PoseTriggerReplay.Annotation annotation =
        new PoseTriggerReplay.Annotation(
            millisToNanos(parseLong(required(options, "safe-arm-start-ms"), "safe-arm-start-ms")),
            millisToNanos(
                parseLong(
                    options.getOrDefault(
                        "preferred-arm-ms", required(options, "safe-arm-start-ms")),
                    "preferred-arm-ms")),
            millisToNanos(parseLong(required(options, "takeaway-ms"), "takeaway-ms")),
            millisToNanos(parseLong(required(options, "startup-budget-ms"), "startup-budget-ms")),
            parseForbidden(options.getOrDefault("must-not-arm-ms", "")));
    PoseTriggerReplay.Result result =
        PoseTriggerReplay.evaluate(
            PoseTriggerController.Config.defaultsForFiveFramesPerSecond(),
            observations,
            annotation);
    return toJson(observations.size(), result);
  }

  private static Map<String, String> parseOptions(String[] arguments) {
    Map<String, String> options = new HashMap<>();
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

  private static List<PoseTriggerReplay.Interval> parseForbidden(String value) {
    if (value.isEmpty()) {
      return List.of();
    }
    List<PoseTriggerReplay.Interval> intervals = new ArrayList<>();
    for (String encoded : value.split(",", -1)) {
      String[] endpoints = encoded.split(":", -1);
      if (endpoints.length != 2) {
        throw new IllegalArgumentException("must-not-arm-ms entries must use start:end");
      }
      intervals.add(
          new PoseTriggerReplay.Interval(
              millisToNanos(parseLong(endpoints[0], "must-not-arm-ms start")),
              millisToNanos(parseLong(endpoints[1], "must-not-arm-ms end"))));
    }
    return List.copyOf(intervals);
  }

  private static long parseLong(String value, String name) {
    try {
      return Long.parseLong(value);
    } catch (NumberFormatException exception) {
      throw new IllegalArgumentException("invalid integer for " + name, exception);
    }
  }

  private static long millisToNanos(long millis) {
    return Math.multiplyExact(millis, MILLIS_TO_NANOS);
  }

  private static String toJson(int observationCount, PoseTriggerReplay.Result result) {
    return "{"
        + "\"schema_version\":1,"
        + "\"observation_count\":"
        + observationCount
        + ",\"arm_request_ns\":"
        + optionalLongJson(result.armRequestNs())
        + ",\"arm_offset_from_preferred_ns\":"
        + optionalLongJson(result.armOffsetFromPreferredNs())
        + ",\"high_speed_ready_ns\":"
        + optionalLongJson(result.highSpeedReadyNs())
        + ",\"ready_lead_before_takeaway_ns\":"
        + optionalLongJson(result.readyLeadBeforeTakeawayNs())
        + ",\"armed_before_safe_window\":"
        + result.armedBeforeSafeWindow()
        + ",\"armed_in_forbidden_interval\":"
        + result.armedInForbiddenInterval()
        + ",\"ready_by_takeaway\":"
        + result.readyByTakeaway()
        + ",\"passed\":"
        + result.passed()
        + ",\"outcome\":\""
        + result.outcome().name().toLowerCase(Locale.ROOT)
        + "\",\"arm_request_count\":"
        + result.armRequestCount()
        + ",\"final_state\":\""
        + result.finalState().name().toLowerCase(Locale.ROOT)
        + "\"}";
  }

  private static String optionalLongJson(OptionalLong value) {
    return value.isPresent() ? "\"" + value.orElseThrow() + "\"" : "null";
  }
}
