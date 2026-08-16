package com.agoessling.swingcapture;

/** One satisfiable inclusive HTTP byte range. */
public final class HttpByteRange {
  private final long start;
  private final long end;

  private HttpByteRange(long start, long end) {
    this.start = start;
    this.end = end;
  }

  public static HttpByteRange entireFile(long length) {
    requireNonempty(length);
    return new HttpByteRange(0, length - 1);
  }

  public static HttpByteRange parse(String value, long length) {
    requireNonempty(length);
    if (value == null || !value.startsWith("bytes=") || value.indexOf(',') >= 0) {
      throw new IllegalArgumentException("Only one bytes range is supported");
    }
    String specification = value.substring("bytes=".length()).trim();
    int separator = specification.indexOf('-');
    if (separator < 0 || separator != specification.lastIndexOf('-')) {
      throw new IllegalArgumentException("Malformed bytes range");
    }
    String startText = specification.substring(0, separator).trim();
    String endText = specification.substring(separator + 1).trim();
    try {
      if (startText.isEmpty()) {
        long suffixLength = Long.parseLong(endText);
        if (suffixLength <= 0) {
          throw new IllegalArgumentException("Suffix range must be positive");
        }
        long selectedLength = Math.min(suffixLength, length);
        return new HttpByteRange(length - selectedLength, length - 1);
      }
      long start = Long.parseLong(startText);
      if (start < 0 || start >= length) {
        throw new IllegalArgumentException("Range starts outside the file");
      }
      long end = endText.isEmpty() ? length - 1 : Long.parseLong(endText);
      if (end < start) {
        throw new IllegalArgumentException("Range end precedes its start");
      }
      return new HttpByteRange(start, Math.min(end, length - 1));
    } catch (NumberFormatException invalidNumber) {
      throw new IllegalArgumentException("Malformed bytes range", invalidNumber);
    }
  }

  public long start() {
    return start;
  }

  public long end() {
    return end;
  }

  public long length() {
    return end - start + 1;
  }

  private static void requireNonempty(long length) {
    if (length <= 0) {
      throw new IllegalArgumentException("A byte range requires a nonempty file");
    }
  }
}
