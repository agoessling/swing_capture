package com.agoessling.swingcapture;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/** Maps an HTTP path to a packaged web asset without permitting path traversal. */
public final class StaticWebAssetRoute {
  private static final int MAXIMUM_PATH_BYTES = 2_048;

  /** Immutable routing decision for a static HTTP request. */
  public record Result(String assetPath, String contentType, String cacheControl) {}

  private StaticWebAssetRoute() {}

  /** Returns the asset to serve, or {@code null} when the path is not a web UI route. */
  public static Result resolve(String requestPath) {
    String decoded = decodePath(requestPath);
    if (decoded == null || !decoded.startsWith("/") || decoded.indexOf('\\') >= 0) {
      return null;
    }

    StringBuilder normalized = new StringBuilder();
    String[] segments = decoded.substring(1).split("/", -1);
    for (String segment : segments) {
      if (segment.isEmpty()) {
        continue;
      }
      if (segment.equals(".") || segment.equals("..") || !isSafeSegment(segment)) {
        return null;
      }
      if (!normalized.isEmpty()) {
        normalized.append('/');
      }
      normalized.append(segment);
    }

    String path = normalized.toString();
    if (path.isEmpty() || path.equals("index.html")) {
      return result("index.html", true);
    }
    if (path.equals("api") || path.startsWith("api/")) {
      return null;
    }

    String finalSegment = path.substring(path.lastIndexOf('/') + 1);
    if (!finalSegment.contains(".")) {
      return result("index.html", true);
    }
    return result(path, false);
  }

  private static Result result(String assetPath, boolean document) {
    return new Result(
        assetPath,
        contentType(assetPath),
        document ? "no-store" : cacheControl(assetPath));
  }

  private static String cacheControl(String assetPath) {
    String extension = extension(assetPath);
    if (extension.equals("js") || extension.equals("css") || extension.equals("json")) {
      // The production bundle does not currently use content-hashed filenames.
      return "no-cache";
    }
    return "public, max-age=86400";
  }

  private static String contentType(String assetPath) {
    return switch (extension(assetPath)) {
      case "html" -> "text/html; charset=utf-8";
      case "css" -> "text/css; charset=utf-8";
      case "js", "mjs" -> "text/javascript; charset=utf-8";
      case "json", "map" -> "application/json; charset=utf-8";
      case "txt" -> "text/plain; charset=utf-8";
      case "svg" -> "image/svg+xml";
      case "png" -> "image/png";
      case "jpg", "jpeg" -> "image/jpeg";
      case "gif" -> "image/gif";
      case "webp" -> "image/webp";
      case "avif" -> "image/avif";
      case "ico" -> "image/x-icon";
      case "woff" -> "font/woff";
      case "woff2" -> "font/woff2";
      case "ttf" -> "font/ttf";
      case "wasm" -> "application/wasm";
      default -> "application/octet-stream";
    };
  }

  private static String extension(String assetPath) {
    int separator = assetPath.lastIndexOf('.');
    return separator < 0
        ? ""
        : assetPath.substring(separator + 1).toLowerCase(Locale.ROOT);
  }

  private static boolean isSafeSegment(String segment) {
    for (int index = 0; index < segment.length(); ++index) {
      char value = segment.charAt(index);
      if (!(value >= 'a' && value <= 'z')
          && !(value >= 'A' && value <= 'Z')
          && !(value >= '0' && value <= '9')
          && value != '.'
          && value != '_'
          && value != '-'
          && value != '~') {
        return false;
      }
    }
    return true;
  }

  private static String decodePath(String value) {
    if (value == null || value.isEmpty() || value.length() > MAXIMUM_PATH_BYTES) {
      return null;
    }
    ByteArrayOutputStream bytes = new ByteArrayOutputStream(value.length());
    for (int index = 0; index < value.length(); ++index) {
      char character = value.charAt(index);
      if (character == '%') {
        if (index + 2 >= value.length()) {
          return null;
        }
        int high = Character.digit(value.charAt(index + 1), 16);
        int low = Character.digit(value.charAt(index + 2), 16);
        if (high < 0 || low < 0) {
          return null;
        }
        bytes.write((high << 4) | low);
        index += 2;
      } else if (character <= 0x7f) {
        bytes.write(character);
      } else {
        bytes.writeBytes(String.valueOf(character).getBytes(StandardCharsets.UTF_8));
      }
    }
    try {
      CharBuffer decoded =
          StandardCharsets.UTF_8
              .newDecoder()
              .onMalformedInput(CodingErrorAction.REPORT)
              .onUnmappableCharacter(CodingErrorAction.REPORT)
              .decode(ByteBuffer.wrap(bytes.toByteArray()));
      return decoded.toString();
    } catch (CharacterCodingException malformed) {
      return null;
    }
  }
}
