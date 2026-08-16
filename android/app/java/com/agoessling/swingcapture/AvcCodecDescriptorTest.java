package com.agoessling.swingcapture;

import java.nio.ByteBuffer;
import java.util.Arrays;

/** Deterministic host tests for RFC 6381 AVC derivation and MediaCodec consistency checks. */
public final class AvcCodecDescriptorTest {
  private AvcCodecDescriptorTest() {}

  public static void main(String[] args) {
    parsesFourByteAnnexBWithoutMutatingInput();
    parsesThreeByteAnnexBAfterAnotherNal();
    parsesBareAndLengthPrefixedSps();
    parsesAndCrossChecksAvcConfigurationRecord();
    validatesAndroidProfileAndLevel();
    rejectsMalformedOrInconsistentData();
  }

  private static void parsesFourByteAnnexBWithoutMutatingInput() {
    ByteBuffer csd = ByteBuffer.wrap(new byte[] {9, 0, 0, 0, 1, 0x67, 0x64, 0, 0x34, 1, 2});
    csd.position(1);
    int originalPosition = csd.position();
    AvcCodecDescriptor descriptor = AvcCodecDescriptor.parse(csd, 0x08, 0x10000);
    check(descriptor.rfc6381Codec().equals("avc1.640034"), "High 5.2 codec string");
    check(descriptor.profileIdc() == 100, "High profile_idc");
    check(descriptor.levelIdc() == 52, "level 5.2 level_idc");
    check(csd.position() == originalPosition, "parser must preserve the source position");
  }

  private static void parsesThreeByteAnnexBAfterAnotherNal() {
    byte[] csd = {0, 0, 1, 0x68, 1, 2, 0, 0, 1, 0x67, 0x42, (byte) 0xe0, 0x1f};
    check(
        AvcCodecDescriptor.parse(ByteBuffer.wrap(csd), null, null)
            .rfc6381Codec()
            .equals("avc1.42e01f"),
        "Annex-B scan must find the SPS after PPS data");
  }

  private static void parsesBareAndLengthPrefixedSps() {
    byte[] sps = {0x67, 0x4d, 0x40, 0x29, 9};
    check(
        AvcCodecDescriptor.parse(ByteBuffer.wrap(sps), null, null)
            .rfc6381Codec()
            .equals("avc1.4d4029"),
        "bare SPS");
    byte[] lengthPrefixed = {0, 0, 0, 5, 0x67, 0x4d, 0x40, 0x29, 9};
    check(
        AvcCodecDescriptor.parse(ByteBuffer.wrap(lengthPrefixed), null, null)
            .rfc6381Codec()
            .equals("avc1.4d4029"),
        "length-prefixed SPS");
    byte[] twoByteLengthPrefixed = {
      0, 2, 0x68, 9, 0, 5, 0x67, 0x4d, 0x40, 0x29, 9
    };
    check(
        AvcCodecDescriptor.parse(ByteBuffer.wrap(twoByteLengthPrefixed), null, null)
            .rfc6381Codec()
            .equals("avc1.4d4029"),
        "length-prefixed scan must find an SPS after another NAL");
  }

  private static void parsesAndCrossChecksAvcConfigurationRecord() {
    byte[] avcc = {
      1, 0x64, 0, 0x34, (byte) 0xff, (byte) 0xe1, 0, 5, 0x67, 0x64, 0, 0x34, 9, 1, 0, 0
    };
    check(
        AvcCodecDescriptor.parse(ByteBuffer.wrap(avcc), 0x08, 0x10000)
            .rfc6381Codec()
            .equals("avc1.640034"),
        "AVCDecoderConfigurationRecord");
  }

  private static void validatesAndroidProfileAndLevel() {
    byte[] constrainedBaseline = {0x67, 0x42, 0x50, 0x0b};
    check(
        AvcCodecDescriptor.parse(ByteBuffer.wrap(constrainedBaseline), 0x10000, 0x02)
            .rfc6381Codec()
            .equals("avc1.42500b"),
        "constrained baseline level 1b");
    expectFailure(
        () -> AvcCodecDescriptor.parse(ByteBuffer.wrap(constrainedBaseline), 0x08, 0x02),
        "profile mismatch must fail");
    expectFailure(
        () -> AvcCodecDescriptor.parse(ByteBuffer.wrap(constrainedBaseline), 0x10000, 0x04),
        "level mismatch must fail");
  }

  private static void rejectsMalformedOrInconsistentData() {
    expectFailure(
        () -> AvcCodecDescriptor.parse(ByteBuffer.wrap(new byte[] {0x68, 1, 2, 3}), null, null),
        "PPS is not an SPS");
    byte[] inconsistentAvcc = {
      1, 0x64, 0, 0x33, (byte) 0xff, (byte) 0xe1, 0, 5, 0x67, 0x64, 0, 0x34, 9
    };
    expectFailure(
        () -> AvcCodecDescriptor.parse(ByteBuffer.wrap(inconsistentAvcc), null, null),
        "inconsistent avcC header must fail");
    byte[] oversized = new byte[1024 * 1024 + 1];
    Arrays.fill(oversized, (byte) 1);
    expectFailure(
        () -> AvcCodecDescriptor.parse(ByteBuffer.wrap(oversized), null, null),
        "oversized codec data must fail");
  }

  private static void expectFailure(Runnable operation, String message) {
    try {
      operation.run();
    } catch (IllegalArgumentException expected) {
      return;
    }
    throw new AssertionError(message);
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
