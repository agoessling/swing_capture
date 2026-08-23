package com.agoessling.swingcapture;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import org.json.JSONException;
import org.json.JSONObject;

/** Android JSON adapter kept outside the host-testable peer-clock HTTP transport. */
final class PeerClockJsonResponseParser {
  private PeerClockJsonResponseParser() {}

  static PeerClockClient.ClockResponse parse(byte[] contents) throws IOException {
    try {
      JSONObject body = new JSONObject(new String(contents, StandardCharsets.UTF_8));
      if (body.getInt("schema_version") != 1) {
        throw new IOException("unsupported peer clock schema");
      }
      return new PeerClockClient.ClockResponse(
          body.getString("node_id"),
          parseNanos(body.getString("request_received_elapsed_realtime_ns")),
          parseNanos(body.getString("response_prepared_elapsed_realtime_ns")));
    } catch (JSONException | IllegalArgumentException invalid) {
      throw new IOException("invalid peer clock response", invalid);
    }
  }

  private static long parseNanos(String value) {
    long parsed = Long.parseLong(value);
    if (parsed < 0) {
      throw new IllegalArgumentException("clock timestamp cannot be negative");
    }
    return parsed;
  }
}
