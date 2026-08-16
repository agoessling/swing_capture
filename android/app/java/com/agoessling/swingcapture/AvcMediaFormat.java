package com.agoessling.swingcapture;

import android.media.MediaFormat;
import java.nio.ByteBuffer;

/** Validates an Android AVC output format and derives its browser codec identity from the SPS. */
final class AvcMediaFormat {
  private AvcMediaFormat() {}

  static AvcCodecDescriptor describe(MediaFormat format) {
    String mime = format.getString(MediaFormat.KEY_MIME);
    if (!MediaFormat.MIMETYPE_VIDEO_AVC.equalsIgnoreCase(mime)) {
      throw new IllegalArgumentException("Encoder output is not AVC: " + mime);
    }
    ByteBuffer codecSpecificData = format.getByteBuffer("csd-0");
    if (codecSpecificData == null) {
      throw new IllegalArgumentException("AVC encoder output format is missing csd-0");
    }
    Integer profile =
        format.containsKey(MediaFormat.KEY_PROFILE)
            ? format.getInteger(MediaFormat.KEY_PROFILE)
            : null;
    Integer level =
        format.containsKey(MediaFormat.KEY_LEVEL) ? format.getInteger(MediaFormat.KEY_LEVEL) : null;
    return AvcCodecDescriptor.parse(codecSpecificData, profile, level);
  }
}
