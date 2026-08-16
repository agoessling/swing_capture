package com.agoessling.swingcapture;

/** Hermetic host test for the Android-independent role contract. */
public final class CaptureRoleTest {
  private CaptureRoleTest() {}

  public static void main(String[] args) {
    require(CaptureRole.parse("down_the_line") == CaptureRole.DOWN_THE_LINE);
    require(CaptureRole.parse(" FACE_ON ") == CaptureRole.FACE_ON);
    require(CaptureRole.parse(null) == CaptureRole.UNASSIGNED);
    require("face_on".equals(CaptureRole.FACE_ON.wireName()));

    boolean rejected = false;
    try {
      CaptureRole.parse("Pixel 6");
    } catch (IllegalArgumentException expected) {
      rejected = true;
    }
    require(rejected);
  }

  private static void require(boolean condition) {
    if (!condition) {
      throw new AssertionError("capture-role contract failed");
    }
  }
}
