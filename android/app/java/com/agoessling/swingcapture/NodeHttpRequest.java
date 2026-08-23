package com.agoessling.swingcapture;

import java.nio.charset.StandardCharsets;
import java.util.Map;

/** One validated HTTP/1 request accepted by the node's deliberately small server. */
final class NodeHttpRequest {
  final String method;
  final String path;
  final String query;
  final Map<String, String> headers;
  final byte[] body;

  NodeHttpRequest(
      String method, String path, String query, Map<String, String> headers, byte[] body) {
    this.method = java.util.Objects.requireNonNull(method, "method");
    this.path = java.util.Objects.requireNonNull(path, "path");
    this.query = java.util.Objects.requireNonNull(query, "query");
    this.headers = Map.copyOf(headers);
    this.body = body.clone();
  }

  String bodyText() {
    return new String(body, StandardCharsets.UTF_8);
  }
}
