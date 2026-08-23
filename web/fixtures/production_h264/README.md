# Production-shaped H.264 browser fixtures

These short, synthetic clips are checked in so two-origin HTTP Range and browser playback tests do
not depend on retained phone artifacts. They contain real AVC/H.264 Constrained Baseline video in
an MP4 container and use YUV 4:2:0. The `*-gop30.mp4` production fixtures contain 90 frames with
IDR frames exactly at source frames 0, 30, and 60. That one-second GOP matches Android capture and
forces backward and forward seeks to decode inter-frame dependencies. The older filenames retain
the all-intra comparison fixtures.

They were generated with FFmpeg 8.0.1:

```bash
ffmpeg -f lavfi -i testsrc2=size=320x180:rate=30:duration=3 \
  -an -c:v libx264 -profile:v baseline -level:v 3.0 -pix_fmt yuv420p \
  -crf 24 -x264-params keyint=30:min-keyint=30:scenecut=0 -movflags +faststart \
  down-the-line-gop30.mp4
ffmpeg -f lavfi -i testsrc2=size=320x180:rate=30:duration=3 \
  -vf hue=h=35 -an -c:v libx264 -profile:v baseline -level:v 3.0 -pix_fmt yuv420p \
  -crf 24 -x264-params keyint=30:min-keyint=30:scenecut=0 -movflags +faststart \
  face-on-gop30.mp4
```

FFprobe reports 90 frames and keyframe indices `0,30,60` for each production fixture.

No field recording or third-party media is embedded in these fixtures.
