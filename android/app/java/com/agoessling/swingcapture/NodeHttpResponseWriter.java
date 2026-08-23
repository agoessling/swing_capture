package com.agoessling.swingcapture;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;

/** Android-free HTTP/1 response writer used verbatim by the production node listener. */
final class NodeHttpResponseWriter {
  private static final int COPY_BUFFER_BYTES = 64 * 1024;
  static final long MAXIMUM_PARTIAL_RESPONSE_BYTES = 512L * 1024L;

  private NodeHttpResponseWriter() {}

  static void writeUnauthorized(OutputStream output, boolean head) throws IOException {
    writeErrorJson(
        output,
        401,
        "Unauthorized",
        "a valid bearer control credential is required",
        head,
        Map.of("WWW-Authenticate", "Bearer"));
  }

  static void writeMediaUnauthorized(OutputStream output, boolean head) throws IOException {
    writeErrorJson(
        output,
        401,
        "Unauthorized",
        "a valid bearer credential or scoped media capability is required",
        head,
        Map.of("WWW-Authenticate", "Bearer"));
  }

  static void serveWholeFile(
      OutputStream output,
      File file,
      String contentType,
      boolean head,
      Map<String, String> responseHeaders)
      throws IOException {
    if (!file.isFile()) {
      writeErrorJson(
          output, 404, "Not Found", "artifact not found", head, Map.of());
      return;
    }
    long fileLength = file.length();
    if (fileLength <= 0) {
      writeErrorJson(
          output, 500, "Internal Server Error", "artifact is empty", head, Map.of());
      return;
    }
    writeHeaders(output, 200, "OK", contentType, fileLength, responseHeaders);
    if (head) {
      return;
    }
    try (InputStream input = new BufferedInputStream(new FileInputStream(file))) {
      copyExactly(input, output, fileLength, "Artifact ended before its advertised length");
    }
  }

  static void serveFile(
      NodeHttpRequest request,
      OutputStream output,
      File file,
      String contentType,
      boolean head,
      Map<String, String> responseHeaders)
      throws IOException {
    if (!file.isFile()) {
      writeErrorJson(
          output, 404, "Not Found", "artifact not found", head, Map.of());
      return;
    }
    long fileLength = file.length();
    if (fileLength <= 0) {
      writeErrorJson(
          output, 500, "Internal Server Error", "artifact is empty", head, Map.of());
      return;
    }
    String rangeHeader = request.headers.get("range");
    boolean partial = rangeHeader != null;
    final HttpByteRange range;
    try {
      HttpByteRange requested =
          partial
              ? HttpByteRange.parse(rangeHeader, fileLength)
              : HttpByteRange.entireFile(fileLength);
      range = partial ? requested.limitLength(MAXIMUM_PARTIAL_RESPONSE_BYTES) : requested;
    } catch (IllegalArgumentException invalidRange) {
      writeHeaders(
          output,
          416,
          "Range Not Satisfiable",
          "text/plain",
          0,
          Map.of("Content-Range", "bytes */" + fileLength, "Accept-Ranges", "bytes"));
      return;
    }
    Map<String, String> extra = new HashMap<>(responseHeaders);
    extra.put("Accept-Ranges", "bytes");
    if (partial) {
      extra.put(
          "Content-Range",
          "bytes " + range.start() + "-" + range.end() + "/" + fileLength);
    }
    writeHeaders(
        output,
        partial ? 206 : 200,
        partial ? "Partial Content" : "OK",
        contentType,
        range.length(),
        extra);
    if (head) {
      return;
    }
    try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
      input.seek(range.start());
      byte[] buffer = new byte[COPY_BUFFER_BYTES];
      long remaining = range.length();
      while (remaining > 0) {
        int count = input.read(buffer, 0, (int) Math.min(buffer.length, remaining));
        if (count < 0) {
          throw new IOException("Artifact ended before its advertised range");
        }
        output.write(buffer, 0, count);
        remaining -= count;
      }
    }
  }

  static void writeHeaders(
      OutputStream output,
      int status,
      String reason,
      String contentType,
      long contentLength,
      Map<String, String> extraHeaders)
      throws IOException {
    StringBuilder headers = new StringBuilder();
    headers.append("HTTP/1.1 ").append(status).append(' ').append(reason).append("\r\n");
    headers.append("Content-Type: ").append(contentType).append("\r\n");
    headers.append("Content-Length: ").append(contentLength).append("\r\n");
    headers.append("Access-Control-Allow-Origin: *\r\n");
    headers.append("Access-Control-Allow-Methods: GET, HEAD, POST, PUT, OPTIONS\r\n");
    headers.append("Access-Control-Allow-Headers: Authorization, Range, Content-Type\r\n");
    headers.append(
        "Access-Control-Expose-Headers: Accept-Ranges, Content-Disposition, Content-Length, "
            + "Content-Range, Location, "
            + "X-Swing-Capture-Coordination-Revision, "
            + "X-Swing-Capture-Coordination-Status, "
            + "X-Swing-Capture-Media-Access\r\n");
    headers.append("Connection: close\r\n");
    for (Map.Entry<String, String> entry : extraHeaders.entrySet()) {
      headers.append(entry.getKey()).append(": ").append(entry.getValue()).append("\r\n");
    }
    headers.append("\r\n");
    output.write(headers.toString().getBytes(StandardCharsets.ISO_8859_1));
  }

  private static void writeErrorJson(
      OutputStream output,
      int status,
      String reason,
      String message,
      boolean head,
      Map<String, String> extraHeaders)
      throws IOException {
    byte[] body =
        ("{\"error\":\"" + escapeJsonString(message) + "\"}\n")
            .getBytes(StandardCharsets.UTF_8);
    writeHeaders(
        output,
        status,
        reason,
        "application/json; charset=utf-8",
        body.length,
        extraHeaders);
    if (!head) {
      output.write(body);
    }
  }

  private static String escapeJsonString(String value) {
    StringBuilder escaped = new StringBuilder(value.length());
    for (int index = 0; index < value.length(); ++index) {
      char character = value.charAt(index);
      switch (character) {
        case '\"' -> escaped.append("\\\"");
        case '\\' -> escaped.append("\\\\");
        case '\b' -> escaped.append("\\b");
        case '\f' -> escaped.append("\\f");
        case '\n' -> escaped.append("\\n");
        case '\r' -> escaped.append("\\r");
        case '\t' -> escaped.append("\\t");
        default -> {
          if (character < 0x20) {
            escaped.append(String.format("\\u%04x", (int) character));
          } else {
            escaped.append(character);
          }
        }
      }
    }
    return escaped.toString();
  }

  private static void copyExactly(
      InputStream input, OutputStream output, long length, String shortReadMessage)
      throws IOException {
    byte[] buffer = new byte[COPY_BUFFER_BYTES];
    long remaining = length;
    while (remaining > 0) {
      int count = input.read(buffer, 0, (int) Math.min(buffer.length, remaining));
      if (count < 0) {
        throw new IOException(shortReadMessage);
      }
      output.write(buffer, 0, count);
      remaining -= count;
    }
  }
}
