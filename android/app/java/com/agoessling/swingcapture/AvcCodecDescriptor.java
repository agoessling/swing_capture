package com.agoessling.swingcapture;

import java.nio.ByteBuffer;
import java.util.Locale;
import java.util.Objects;

/** RFC 6381 AVC codec identity derived from the encoder's sequence parameter set. */
public record AvcCodecDescriptor(
    String rfc6381Codec, int profileIdc, int profileCompatibility, int levelIdc) {
  private static final int MAXIMUM_CODEC_SPECIFIC_DATA_BYTES = 1024 * 1024;

  public AvcCodecDescriptor {
    Objects.requireNonNull(rfc6381Codec, "rfc6381Codec");
    requireByte(profileIdc, "profileIdc");
    requireByte(profileCompatibility, "profileCompatibility");
    requireByte(levelIdc, "levelIdc");
    String expected = codecString(profileIdc, profileCompatibility, levelIdc);
    if (!rfc6381Codec.equals(expected)) {
      throw new IllegalArgumentException("RFC 6381 codec does not match its AVC fields");
    }
  }

  /**
   * Parses the SPS from Annex-B, length-prefixed, bare-SPS, or AVCDecoderConfigurationRecord data.
   * Optional Android profile/level values are checked against the bitstream rather than used to
   * synthesize a codec string.
   */
  public static AvcCodecDescriptor parse(
      ByteBuffer codecSpecificData, Integer androidProfile, Integer androidLevel) {
    Objects.requireNonNull(codecSpecificData, "codecSpecificData");
    ByteBuffer source = codecSpecificData.duplicate();
    int size = source.remaining();
    if (size == 0 || size > MAXIMUM_CODEC_SPECIFIC_DATA_BYTES) {
      throw new IllegalArgumentException("AVC codec-specific data has an invalid size");
    }
    byte[] bytes = new byte[size];
    source.get(bytes);

    int[] fields = parseAvcConfigurationRecord(bytes);
    if (fields == null) {
      fields = parseAnnexB(bytes);
    }
    if (fields == null) {
      fields = parseLengthPrefixed(bytes, 4);
    }
    if (fields == null) {
      fields = parseLengthPrefixed(bytes, 2);
    }
    if (fields == null) {
      fields = parseSps(bytes, 0, bytes.length);
    }
    if (fields == null) {
      throw new IllegalArgumentException("AVC codec-specific data does not contain a valid SPS");
    }

    validateAndroidProfile(androidProfile, fields[0]);
    validateAndroidLevel(androidLevel, fields[1], fields[2]);
    return new AvcCodecDescriptor(
        codecString(fields[0], fields[1], fields[2]), fields[0], fields[1], fields[2]);
  }

  private static int[] parseAvcConfigurationRecord(byte[] bytes) {
    if (bytes.length < 7 || unsigned(bytes[0]) != 1) {
      return null;
    }
    int sequenceParameterSetCount = unsigned(bytes[5]) & 0x1f;
    int offset = 6;
    for (int index = 0; index < sequenceParameterSetCount; ++index) {
      if (offset + 2 > bytes.length) {
        return null;
      }
      int length = (unsigned(bytes[offset]) << 8) | unsigned(bytes[offset + 1]);
      offset += 2;
      if (length <= 0 || offset + length > bytes.length) {
        return null;
      }
      int[] fields = parseSps(bytes, offset, length);
      if (fields != null) {
        if (fields[0] != unsigned(bytes[1])
            || fields[1] != unsigned(bytes[2])
            || fields[2] != unsigned(bytes[3])) {
          throw new IllegalArgumentException(
              "AVC configuration record disagrees with its sequence parameter set");
        }
        return fields;
      }
      offset += length;
    }
    return null;
  }

  private static int[] parseAnnexB(byte[] bytes) {
    for (int index = 0; index + 3 < bytes.length; ++index) {
      int startCodeBytes = 0;
      if (bytes[index] == 0 && bytes[index + 1] == 0 && bytes[index + 2] == 1) {
        startCodeBytes = 3;
      } else if (index + 4 < bytes.length
          && bytes[index] == 0
          && bytes[index + 1] == 0
          && bytes[index + 2] == 0
          && bytes[index + 3] == 1) {
        startCodeBytes = 4;
      }
      if (startCodeBytes != 0) {
        int[] fields =
            parseSps(bytes, index + startCodeBytes, bytes.length - index - startCodeBytes);
        if (fields != null) {
          return fields;
        }
        index += startCodeBytes - 1;
      }
    }
    return null;
  }

  private static int[] parseLengthPrefixed(byte[] bytes, int lengthBytes) {
    int offset = 0;
    while (offset + lengthBytes < bytes.length) {
      long length = 0;
      for (int index = 0; index < lengthBytes; ++index) {
        length = (length << 8) | unsigned(bytes[offset + index]);
      }
      offset += lengthBytes;
      if (length <= 0 || length > bytes.length - offset) {
        return null;
      }
      int[] fields = parseSps(bytes, offset, (int) length);
      if (fields != null) {
        return fields;
      }
      offset += (int) length;
    }
    return null;
  }

  private static int[] parseSps(byte[] bytes, int offset, int length) {
    if (offset < 0 || length < 4 || offset > bytes.length - length) {
      return null;
    }
    if ((unsigned(bytes[offset]) & 0x1f) != 7) {
      return null;
    }
    return new int[] {
      unsigned(bytes[offset + 1]), unsigned(bytes[offset + 2]), unsigned(bytes[offset + 3])
    };
  }

  private static void validateAndroidProfile(Integer androidProfile, int profileIdc) {
    if (androidProfile == null) {
      return;
    }
    int expectedProfileIdc =
        switch (androidProfile) {
          case 0x01, 0x10000 -> 66; // Baseline and Constrained Baseline.
          case 0x02 -> 77; // Main.
          case 0x04 -> 88; // Extended.
          case 0x08, 0x80000 -> 100; // High and Constrained High.
          case 0x10 -> 110; // High 10.
          case 0x20 -> 122; // High 4:2:2.
          case 0x40 -> 244; // High 4:4:4 Predictive.
          default ->
              throw new IllegalArgumentException(
                  "Unsupported Android AVC profile value " + androidProfile);
        };
    if (profileIdc != expectedProfileIdc) {
      throw new IllegalArgumentException(
          "Android AVC profile disagrees with SPS profile_idc " + profileIdc);
    }
  }

  private static void validateAndroidLevel(
      Integer androidLevel, int profileCompatibility, int levelIdc) {
    if (androidLevel == null) {
      return;
    }
    if (androidLevel == 0x02) { // AVCLevel1b is level 1.1 with constraint_set3_flag.
      if (levelIdc != 11 || (profileCompatibility & 0x10) == 0) {
        throw new IllegalArgumentException("Android AVC level 1b disagrees with the SPS");
      }
      return;
    }
    int ordinal = Integer.numberOfTrailingZeros(androidLevel);
    if (androidLevel <= 0 || Integer.bitCount(androidLevel) != 1 || ordinal > 19) {
      throw new IllegalArgumentException("Unsupported Android AVC level value " + androidLevel);
    }
    int[] levelIdcs = {
      10, -1, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61, 62
    };
    int expectedLevelIdc = levelIdcs[ordinal];
    boolean levelOneOneClaimsLevelOneB =
        androidLevel == 0x04 && levelIdc == 11 && (profileCompatibility & 0x10) != 0;
    if (expectedLevelIdc < 0 || levelIdc != expectedLevelIdc || levelOneOneClaimsLevelOneB) {
      throw new IllegalArgumentException(
          "Android AVC level disagrees with SPS level_idc " + levelIdc);
    }
  }

  private static String codecString(int profileIdc, int profileCompatibility, int levelIdc) {
    return String.format(
        Locale.ROOT, "avc1.%02x%02x%02x", profileIdc, profileCompatibility, levelIdc);
  }

  private static int unsigned(byte value) {
    return Byte.toUnsignedInt(value);
  }

  private static void requireByte(int value, String name) {
    if (value < 0 || value > 255) {
      throw new IllegalArgumentException(name + " must fit in an unsigned byte");
    }
  }
}
