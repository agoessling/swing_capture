package com.agoessling.swingcapture;

/** Host-side tests for capture-profile persistence values. */
public final class CaptureProfileTest {
  private CaptureProfileTest() {}

  public static void main(String[] arguments) {
    assert CaptureProfile.standard() == CaptureProfile.HD_240;
    assert CaptureProfile.parse(null) == CaptureProfile.HD_240;
    assert CaptureProfile.parse("") == CaptureProfile.HD_240;
    assert CaptureProfile.parse("1080p240") == CaptureProfile.FULL_HD_240;
    assert CaptureProfile.parse("720p240") == CaptureProfile.HD_240;
    assert CaptureProfile.HD_240.width() == 1280;
    assert CaptureProfile.HD_240.height() == 720;
    assert CaptureProfile.HD_240.bitrateBitsPerSecond() == 12_000_000;

    boolean rejected = false;
    try {
      CaptureProfile.parse("pixel5a");
    } catch (IllegalArgumentException expected) {
      rejected = true;
    }
    assert rejected;
  }
}
