"""Hermetic coverage for scheduling and MediaPipe adapter boundaries."""

from __future__ import annotations

import io
import json
import unittest
from dataclasses import dataclass
from pathlib import Path
from typing import ClassVar, cast, final

from tools.pose_inference import contract, mediapipe_runtime

FAKE_POSITION_PROPERTY_ID = 3


@final
class _FakeReader:
    def __init__(
        self,
        duration_ms: int,
        missing_at: int | None = None,
        source_timestamps: dict[int, int] | None = None,
    ) -> None:
        self._duration_ms = duration_ms
        self._missing_at = missing_at
        self._source_timestamps = source_timestamps or {}
        self.requested: list[int] = []

    @property
    def duration_ms(self) -> int:
        return self._duration_ms

    def read_at(self, timestamp_ms: int) -> contract.VideoFrame | None:
        self.requested.append(timestamp_ms)
        if timestamp_ms == self._missing_at:
            return None
        source_timestamp_ms = self._source_timestamps.get(timestamp_ms, timestamp_ms + 7)
        return contract.VideoFrame(
            pixels=f"rgb-{source_timestamp_ms}",
            width=640,
            height=360,
            source_timestamp_ms=source_timestamp_ms,
        )


@final
class _FakeLandmarker:
    def __init__(self) -> None:
        self.timestamps: list[int] = []

    def detect_for_video(self, rgb_pixels: object, timestamp_ms: int) -> tuple[contract.Pose, ...]:
        self.timestamps.append(timestamp_ms)
        self._assert_pixels(rgb_pixels, timestamp_ms)
        landmark = contract.Landmark(
            x=timestamp_ms / 1000.0,
            y=0.25,
            z=-0.5,
            visibility=0.9,
            presence=0.8,
        )
        return (
            contract.Pose(
                normalized_landmarks=(landmark,),
                world_landmarks=(landmark,),
            ),
        )

    @staticmethod
    def _assert_pixels(rgb_pixels: object, timestamp_ms: int) -> None:
        if rgb_pixels != f"rgb-{timestamp_ms}":
            message = "coordinator passed pixels from the wrong sample"
            raise AssertionError(message)


@final
class _FakeNativeLandmarker:
    def __init__(self, result: object) -> None:
        self.result = result
        self.calls: list[tuple[object, int]] = []
        self.closed = False

    def detect_for_video(self, image: object, timestamp_ms: int) -> object:
        self.calls.append((image, timestamp_ms))
        return self.result

    def close(self) -> None:
        self.closed = True


@final
@dataclass
class _FakeLandmark:
    x: float
    y: float
    z: float
    visibility: float | None = None
    presence: float | None = None


@final
@dataclass
class _FakeResult:
    pose_landmarks: list[list[_FakeLandmark]]
    pose_world_landmarks: list[list[_FakeLandmark]]


@final
class _FakeBaseOptions:
    def __init__(self, *, model_asset_path: str) -> None:
        self.model_asset_path = model_asset_path


@final
class _FakeOptions:
    def __init__(
        self,
        *,
        base_options: _FakeBaseOptions,
        running_mode: str,
        num_poses: int,
        output_segmentation_masks: bool,
    ) -> None:
        self.base_options = base_options
        self.running_mode = running_mode
        self.num_poses = num_poses
        self.output_segmentation_masks = output_segmentation_masks


@final
class _FakeImage:
    def __init__(self, *, image_format: str, data: object) -> None:
        self.image_format = image_format
        self.data = data


@final
class _FakePoseLandmarkerFactory:
    native: ClassVar[_FakeNativeLandmarker | None] = None
    options: ClassVar[_FakeOptions | None] = None

    @classmethod
    def create_from_options(cls, options: _FakeOptions) -> _FakeNativeLandmarker:
        cls.options = options
        if cls.native is None:
            message = "fake native landmarker was not initialized"
            raise AssertionError(message)
        return cls.native


@final
class _FakeRunningMode:
    VIDEO = "video"


@final
class _FakeVision:
    PoseLandmarkerOptions = _FakeOptions
    PoseLandmarker = _FakePoseLandmarkerFactory
    RunningMode = _FakeRunningMode


@final
class _FakeTasks:
    BaseOptions = _FakeBaseOptions
    vision = _FakeVision


@final
class _FakeImageFormat:
    SRGB = "srgb"


@final
class _FakeMediaPipe:
    tasks = _FakeTasks
    Image = _FakeImage
    ImageFormat = _FakeImageFormat


@final
class _FakePixels:
    shape = (360, 640, 3)


@final
class _FakeCapture:
    def __init__(self, timestamps_ms: list[float]) -> None:
        self.timestamps_ms = timestamps_ms
        self.position_ms = float("nan")
        self.frame_index = 0
        self.released = False

    @staticmethod
    def isOpened() -> bool:  # noqa: N802
        return True

    def get(self, property_id: int) -> float:
        if property_id != FAKE_POSITION_PROPERTY_ID:
            message = f"unexpected fake capture property: {property_id}"
            raise AssertionError(message)
        return self.position_ms

    def set(self, property_id: int, value: float) -> bool:
        message = (
            f"timestamp-preserving decoder unexpectedly sought property {property_id} to {value}"
        )
        raise AssertionError(message)

    def read(self) -> tuple[bool, _FakePixels | None]:
        if self.frame_index >= len(self.timestamps_ms):
            return False, None
        self.position_ms = self.timestamps_ms[self.frame_index]
        self.frame_index += 1
        return True, _FakePixels()

    def release(self) -> None:
        self.released = True


@final
class _FakeCv2:
    CAP_PROP_POS_MSEC = FAKE_POSITION_PROPERTY_ID
    COLOR_BGR2RGB = 4

    def __init__(self, timestamps_by_open: list[list[float]] | None = None) -> None:
        self.timestamps_by_open = timestamps_by_open or [
            [index * 1_000.0 / 30.0 for index in range(30)]
        ]
        self.captures: list[_FakeCapture] = []

    def VideoCapture(self, _path: str) -> _FakeCapture:  # noqa: N802
        index = min(len(self.captures), len(self.timestamps_by_open) - 1)
        capture = _FakeCapture(self.timestamps_by_open[index])
        self.captures.append(capture)
        return capture

    @staticmethod
    def cvtColor(pixels: _FakePixels, conversion: int) -> _FakePixels:  # noqa: N802
        if conversion != _FakeCv2.COLOR_BGR2RGB:
            message = "unexpected fake color conversion"
            raise AssertionError(message)
        return pixels


def _fake_mediapipe(result: _FakeResult) -> mediapipe_runtime.MediaPipeModule:
    _FakePoseLandmarkerFactory.native = _FakeNativeLandmarker(result)
    _FakePoseLandmarkerFactory.options = None
    return cast("mediapipe_runtime.MediaPipeModule", cast("object", _FakeMediaPipe()))


class PoseInferenceContractTest(unittest.TestCase):
    """Exercise the dependency-free sampling and serialization contract."""

    def test_five_hertz_schedule_is_half_open_and_exact(self) -> None:
        """Schedule exact 200 ms intervals and exclude the media endpoint."""
        self.assertEqual((0, 200, 400, 600, 800), contract.sample_timestamps(1000))
        self.assertEqual((0, 200, 400, 600, 800, 1000), contract.sample_timestamps(1001))
        self.assertEqual((), contract.sample_timestamps(0))
        with self.assertRaises(ValueError):
            contract.sample_timestamps(-1)
        with self.assertRaises(ValueError):
            contract.sample_timestamps(1000, 0)

    def test_coordinator_emits_one_stable_record_per_sample(self) -> None:
        """Preserve scheduled times and stable, versioned NDJSON fields."""
        reader = _FakeReader(601)
        landmarker = _FakeLandmarker()
        output = io.StringIO()
        count = contract.run_inference(reader, landmarker, output)

        self.assertEqual(4, count)
        self.assertEqual([0, 200, 400, 600], reader.requested)
        self.assertEqual([7, 207, 407, 607], landmarker.timestamps)
        records = [
            cast("dict[str, object]", json.loads(line)) for line in output.getvalue().splitlines()
        ]
        self.assertEqual([0, 1, 2, 3], [value["sample_index"] for value in records])
        self.assertEqual([0, 200, 400, 600], [value["timestamp_ms"] for value in records])
        self.assertEqual([7, 207, 407, 607], [value["source_timestamp_ms"] for value in records])
        self.assertEqual({"height": 360, "width": 640}, records[0]["image"])
        self.assertEqual(1, records[0]["schema_version"])
        poses = cast("list[dict[str, object]]", records[0]["poses"])
        normalized = cast("list[dict[str, object]]", poses[0]["normalized_landmarks"])
        self.assertEqual(0.9, normalized[0]["visibility"])

    def test_coordinator_does_not_silently_drop_decode_failures(self) -> None:
        """Reject missing scheduled frames instead of skewing evaluation data."""
        with self.assertRaisesRegex(RuntimeError, "400 ms"):
            contract.run_inference(
                _FakeReader(600, missing_at=400), _FakeLandmarker(), io.StringIO()
            )

    def test_coordinator_rejects_unfaithful_source_timestamps(self) -> None:
        """Prevent a decoder from relabeling preceding or repeated frames as samples."""
        cases = (
            (_FakeReader(1, source_timestamps={0: -1}), "before requested"),
            (
                _FakeReader(201, source_timestamps={0: 200, 200: 200}),
                "non-increasing",
            ),
        )
        for reader, expected in cases:
            with (
                self.subTest(expected=expected),
                self.assertRaisesRegex(RuntimeError, expected),
            ):
                contract.run_inference(reader, _FakeLandmarker(), io.StringIO())

    def test_mediapipe_adapter_uses_video_mode_and_maps_both_coordinate_spaces(self) -> None:
        """Configure tracking mode and retain normalized and world landmarks."""
        normalized = _FakeLandmark(x=0.1, y=0.2, z=-0.3, visibility=0.9, presence=0.8)
        world = _FakeLandmark(x=1.0, y=2.0, z=3.0)
        result = _FakeResult(pose_landmarks=[[normalized]], pose_world_landmarks=[[world]])
        mediapipe_module = _fake_mediapipe(result)
        adapter = mediapipe_runtime.MediaPipePoseLandmarker(mediapipe_module, Path("model.task"))
        poses = adapter.detect_for_video("rgb", 200)

        options = _FakePoseLandmarkerFactory.options
        if options is None:
            self.fail("adapter did not create MediaPipe options")
        self.assertEqual("video", options.running_mode)
        self.assertEqual("model.task", options.base_options.model_asset_path)
        self.assertFalse(options.output_segmentation_masks)
        self.assertEqual(0.1, poses[0].normalized_landmarks[0].x)
        self.assertEqual(3.0, poses[0].world_landmarks[0].z)
        self.assertIsNone(poses[0].world_landmarks[0].visibility)
        native = _FakePoseLandmarkerFactory.native
        if native is None:
            self.fail("adapter did not create a native landmarker")
        self.assertEqual(200, native.calls[0][1])
        adapter.close()
        self.assertTrue(native.closed)

    def test_mediapipe_adapter_rejects_mismatched_pose_counts(self) -> None:
        """Reject structurally inconsistent native task results."""
        result = _FakeResult(
            pose_landmarks=[[_FakeLandmark(x=0, y=0, z=0)]],
            pose_world_landmarks=[],
        )
        adapter = mediapipe_runtime.MediaPipePoseLandmarker(
            _fake_mediapipe(result), Path("model.task")
        )
        with self.assertRaisesRegex(RuntimeError, "mismatched"):
            adapter.detect_for_video("rgb", 0)

    def test_opencv_adapter_decodes_forward_and_reports_cfr_geometry(self) -> None:
        """Derive duration and decode requested CFR media without timestamp seeking."""
        cv2_module = _FakeCv2()
        reader = mediapipe_runtime.OpenCvVideoReader(
            cast("mediapipe_runtime.Cv2Module", cast("object", cv2_module)),
            Path("clip.mp4"),
        )
        self.assertEqual(967, reader.duration_ms)
        frame = reader.read_at(200)
        if frame is None:
            self.fail("fake decoder unexpectedly returned no frame")
        self.assertEqual(640, frame.width)
        self.assertEqual(360, frame.height)
        self.assertEqual(200, frame.source_timestamp_ms)
        reader.close()
        self.assertEqual(2, len(cv2_module.captures))
        self.assertTrue(all(capture.released for capture in cv2_module.captures))

    def test_opencv_duration_excludes_ntsc_display_tail_without_a_frame(self) -> None:
        """Do not schedule a 24.000s read after the last decoded NTSC frame."""
        timestamps = [index * 1_001.0 / 30.0 for index in range(720)]
        cv2_module = _FakeCv2([timestamps])
        reader = mediapipe_runtime.OpenCvVideoReader(
            cast("mediapipe_runtime.Cv2Module", cast("object", cv2_module)),
            Path("clip.mp4"),
        )
        self.assertEqual(23_991, reader.duration_ms)
        self.assertEqual(23_800, contract.sample_timestamps(reader.duration_ms)[-1])
        reader.close()

    def test_opencv_adapter_preserves_variable_frame_timestamps(self) -> None:
        """Select first-at-or-after VFR frames and retain their decoded PTS."""
        timestamps = [0.0, 41.0, 83.0, 150.0, 207.0, 390.0, 401.0, 799.0, 1_005.0]
        cv2_module = _FakeCv2([timestamps])
        reader = mediapipe_runtime.OpenCvVideoReader(
            cast("mediapipe_runtime.Cv2Module", cast("object", cv2_module)),
            Path("variable.mp4"),
        )

        self.assertEqual(1_006, reader.duration_ms)
        frames = [reader.read_at(timestamp) for timestamp in (0, 200, 400, 800)]
        self.assertTrue(all(frame is not None for frame in frames))
        self.assertEqual(
            [0, 207, 401, 1_005],
            [frame.source_timestamp_ms for frame in frames if frame is not None],
        )
        reader.close()

    def test_opencv_adapter_reopens_for_backward_reads(self) -> None:
        """Preserve the timestamp-addressable API without backend time seeking."""
        timestamps = [0.0, 150.0, 401.0, 799.0]
        cv2_module = _FakeCv2([timestamps])
        reader = mediapipe_runtime.OpenCvVideoReader(
            cast("mediapipe_runtime.Cv2Module", cast("object", cv2_module)),
            Path("variable.mp4"),
        )

        later = reader.read_at(400)
        earlier = reader.read_at(100)
        self.assertEqual(401, later.source_timestamp_ms if later is not None else None)
        self.assertEqual(150, earlier.source_timestamp_ms if earlier is not None else None)
        self.assertEqual(3, len(cv2_module.captures))
        reader.close()

    def test_opencv_adapter_rejects_one_frame_for_two_sample_deadlines(self) -> None:
        """Do not conceal a source cadence too sparse for the requested replay rate."""
        timestamps = [0.0, 300.0, 600.0]
        reader = mediapipe_runtime.OpenCvVideoReader(
            cast(
                "mediapipe_runtime.Cv2Module",
                cast("object", _FakeCv2([timestamps])),
            ),
            Path("sparse.mp4"),
        )
        first = reader.read_at(200)
        self.assertEqual(300, first.source_timestamp_ms if first is not None else None)
        with self.assertRaisesRegex(RuntimeError, "distinct frames"):
            reader.read_at(250)
        reader.close()

    def test_opencv_adapter_rejects_untrustworthy_decoded_timestamps(self) -> None:
        """Fail closed instead of silently treating a broken timebase as CFR."""
        cases = (
            ([0.0, float("nan")], "finite nonnegative"),
            ([0.0, 0.0], "must increase"),
            ([0.0, 0.4], "millisecond resolution"),
        )
        for timestamps, expected in cases:
            with (
                self.subTest(timestamps=timestamps),
                self.assertRaisesRegex(RuntimeError, expected),
            ):
                mediapipe_runtime.OpenCvVideoReader(
                    cast(
                        "mediapipe_runtime.Cv2Module",
                        cast("object", _FakeCv2([timestamps])),
                    ),
                    Path("invalid.mp4"),
                )

    def test_opencv_adapter_revalidates_timestamps_during_replay(self) -> None:
        """Detect decode-order regressions even when the initial scan was sound."""
        cv2_module = _FakeCv2(
            [
                [0.0, 100.0, 150.0, 200.0],
                [0.0, 100.0, 50.0, 200.0],
            ]
        )
        reader = mediapipe_runtime.OpenCvVideoReader(
            cast("mediapipe_runtime.Cv2Module", cast("object", cv2_module)),
            Path("unstable.mp4"),
        )
        self.assertIsNotNone(reader.read_at(0))
        with self.assertRaisesRegex(RuntimeError, "must increase"):
            reader.read_at(150)
        reader.close()

    def test_runtime_dependency_error_is_actionable(self) -> None:
        """Explain that missing native modules belong in the Bazel graph."""

        def missing(_name: str) -> object:
            raise ModuleNotFoundError

        with self.assertRaisesRegex(RuntimeError, "pip.parse"):
            mediapipe_runtime.load_runtime_modules(missing)


if __name__ == "__main__":
    unittest.main()
