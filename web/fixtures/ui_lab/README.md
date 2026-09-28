# UI lab collection fixture

These two 90-frame VP8 clips are a deterministic, browser-portable review slice from target swing
`S06` in retained field recording `95482d93-f024-400e-9532-5ba7082de05e`. The reviewed impact is
frame 46 in each clip. The per-view source offsets come from
`pose_replay/target_swing_index.json`; aligning each view independently keeps the reviewed impact at
the same media time without assuming that the phones shared a clock epoch.

The fixture is intentionally normal-speed 720p collection evidence displayed using its retained
90-degree rotation and scaled without distortion to 360x640. It is UI development data, not
evidence of high-speed capture or calibrated audio/video synchronization. Every frame is a VP8
keyframe so hermetic Chromium can seek and step it exactly.

Recreate the clips from the retained artifact at the repository root:

```bash
ffmpeg -ss 136.487615 \
  -i artifacts/field_recording_95482d93-f024-400e-9532-5ba7082de05e/down_the_line_pixel5a_video.mp4 \
  -frames:v 90 -vf fps=30,scale=360:640:flags=lanczos -an \
  -c:v libvpx -deadline good -cpu-used 2 -g 1 -b:v 3M \
  web/fixtures/ui_lab/down-the-line-s06.webm

ffmpeg -ss 136.927607 \
  -i artifacts/field_recording_95482d93-f024-400e-9532-5ba7082de05e/face_on_pixel6_video.mp4 \
  -frames:v 90 -vf fps=30,scale=360:640:flags=lanczos -an \
  -c:v libvpx -deadline good -cpu-used 2 -g 1 -b:v 3M \
  web/fixtures/ui_lab/face-on-s06.webm
```
