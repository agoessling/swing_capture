package com.agoessling.swingcapture;

/** Hermetic executable test for browser media range handling. */
public final class HttpByteRangeTest {
  private HttpByteRangeTest() {}

  public static void main(String[] arguments) {
    checkRange(HttpByteRange.entireFile(100), 0, 99);
    checkRange(HttpByteRange.parse("bytes=10-19", 100), 10, 19);
    checkRange(HttpByteRange.parse("bytes=90-", 100), 90, 99);
    checkRange(HttpByteRange.parse("bytes=-8", 100), 92, 99);
    checkRange(HttpByteRange.parse("bytes=95-500", 100), 95, 99);
    expectInvalid("bytes=100-", 100);
    expectInvalid("bytes=20-10", 100);
    expectInvalid("bytes=0-1,4-5", 100);
    expectInvalid("items=0-1", 100);
    expectInvalid("bytes=-0", 100);
  }

  private static void checkRange(HttpByteRange range, long start, long end) {
    if (range.start() != start || range.end() != end || range.length() != end - start + 1) {
      throw new AssertionError("Unexpected parsed range");
    }
  }

  private static void expectInvalid(String value, long length) {
    try {
      HttpByteRange.parse(value, length);
      throw new AssertionError("Expected invalid range: " + value);
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
  }
}
