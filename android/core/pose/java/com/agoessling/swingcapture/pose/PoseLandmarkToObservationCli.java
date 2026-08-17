package com.agoessling.swingcapture.pose;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** Applies the shared Java landmark feature policy to recorded MediaPipe output. */
public final class PoseLandmarkToObservationCli {
  private static final Set<String> ALLOWED_OPTIONS =
      Set.of("landmarks", "observations", "hitting-region", "projection");

  private PoseLandmarkToObservationCli() {}

  public static void main(String[] arguments) throws IOException {
    run(arguments);
  }

  static void run(String[] arguments) throws IOException {
    Map<String, String> options = parseOptions(arguments);
    List<PoseLandmarkFrame> frames =
        PoseLandmarkCsv.parse(
            Files.readString(Path.of(required(options, "landmarks")), StandardCharsets.UTF_8));
    NormalizedHittingRegion region = parseRegion(required(options, "hitting-region"));
    PoseProjection projection = PoseProjection.parse(required(options, "projection"));
    PoseLandmarkObservationExtractor.Config config =
        PoseLandmarkObservationExtractor.Config.defaultsForFiveFramesPerSecond();
    List<PoseTriggerController.Observation> observations = new ArrayList<>(frames.size());
    PoseLandmarkFrame previous = null;
    for (PoseLandmarkFrame frame : frames) {
      observations.add(
          PoseLandmarkObservationExtractor.evaluate(frame, previous, region, config, projection)
              .toControllerObservation());
      previous = frame;
    }
    Files.writeString(
        Path.of(required(options, "observations")),
        PoseObservationCsv.serialize(observations),
        StandardCharsets.UTF_8);
  }

  private static NormalizedHittingRegion parseRegion(String encoded) {
    String[] fields = encoded.split(",", -1);
    if (fields.length != 4) {
      throw new IllegalArgumentException("hitting-region must use left,top,right,bottom");
    }
    try {
      return new NormalizedHittingRegion(
          Double.parseDouble(fields[0]),
          Double.parseDouble(fields[1]),
          Double.parseDouble(fields[2]),
          Double.parseDouble(fields[3]));
    } catch (NumberFormatException exception) {
      throw new IllegalArgumentException("hitting-region contains an invalid number", exception);
    }
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
}
