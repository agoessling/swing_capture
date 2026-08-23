"""Thin adapters around OpenCV decoding and MediaPipe Tasks pose inference."""

from __future__ import annotations

import importlib
import math
from typing import TYPE_CHECKING, Protocol, Self, cast, final

from tools.pose_inference import contract

if TYPE_CHECKING:
    from collections.abc import Callable, Sequence
    from pathlib import Path


class _RgbPixels(Protocol):
    shape: tuple[int, ...]


class _VideoCapture(Protocol):
    def isOpened(self) -> bool: ...  # noqa: N802

    def get(self, property_id: int) -> float: ...

    def read(self) -> tuple[bool, object]: ...

    def release(self) -> None: ...


class _Cv2Module(Protocol):
    CAP_PROP_POS_MSEC: int
    COLOR_BGR2RGB: int

    def VideoCapture(self, path: str) -> _VideoCapture: ...  # noqa: N802

    def cvtColor(self, pixels: object, conversion: int) -> _RgbPixels: ...  # noqa: N802


class _NativeLandmark(Protocol):
    x: float
    y: float
    z: float
    visibility: float | None
    presence: float | None


class _PoseResult(Protocol):
    pose_landmarks: Sequence[Sequence[_NativeLandmark]]
    pose_world_landmarks: Sequence[Sequence[_NativeLandmark]]


class _NativePoseLandmarker(Protocol):
    def detect_for_video(self, image: object, timestamp_ms: int) -> _PoseResult: ...

    def close(self) -> None: ...


class _BaseOptionsFactory(Protocol):
    def __call__(self, *, model_asset_path: str) -> object: ...


class _PoseLandmarkerOptionsFactory(Protocol):
    def __call__(
        self,
        *,
        base_options: object,
        running_mode: object,
        num_poses: int,
        output_segmentation_masks: bool,
    ) -> object: ...


class _PoseLandmarkerFactory(Protocol):
    def create_from_options(self, options: object) -> _NativePoseLandmarker: ...


class _RunningMode(Protocol):
    VIDEO: object


class _VisionTasks(Protocol):
    PoseLandmarkerOptions: _PoseLandmarkerOptionsFactory
    PoseLandmarker: _PoseLandmarkerFactory
    RunningMode: _RunningMode


class _Tasks(Protocol):
    BaseOptions: _BaseOptionsFactory
    vision: _VisionTasks


class _ImageFactory(Protocol):
    def __call__(self, *, image_format: object, data: object) -> object: ...


class _ImageFormat(Protocol):
    SRGB: object


class _MediaPipeModule(Protocol):
    tasks: _Tasks
    Image: _ImageFactory
    ImageFormat: _ImageFormat


Cv2Module = _Cv2Module
MediaPipeModule = _MediaPipeModule
RuntimeModules = tuple[Cv2Module, MediaPipeModule]


def load_runtime_modules(
    importer: Callable[[str], object] = importlib.import_module,
) -> RuntimeModules:
    """Load dependencies supplied by the future Bazel PyPI repository."""
    try:
        cv2_module = importer("cv2")
        mediapipe_module = importer("mediapipe")
    except ModuleNotFoundError as error:
        message = (
            "pose inference runtime dependencies are absent; use the Bazel "
            "pip.parse integration documented in DEPENDENCY_PLAN.md"
        )
        raise RuntimeError(message) from error
    return cast("_Cv2Module", cv2_module), cast("_MediaPipeModule", mediapipe_module)


@final
class OpenCvVideoReader:
    """Decode presentation-ordered frames and retain their source timestamps."""

    def __init__(self, cv2_module: _Cv2Module, input_path: Path) -> None:
        """Open one video and derive its duration from decoded timestamps."""
        self._cv2 = cv2_module
        self._input_path = input_path
        self._closed = False
        self._duration_ms = self._scan_duration_ms()
        self._capture = self._open_capture()
        self._previous_raw_timestamp_ms: float | None = None
        self._previous_timestamp_ms: int | None = None
        self._previous_requested_timestamp_ms: int | None = None

    def _open_capture(self) -> _VideoCapture:
        capture = self._cv2.VideoCapture(str(self._input_path))
        if not capture.isOpened():
            capture.release()
            message = f"cannot open input video: {self._input_path}"
            raise RuntimeError(message)
        return capture

    def _decoded_timestamp(
        self,
        capture: _VideoCapture,
        frame_index: int,
        previous_raw_timestamp_ms: float | None,
        previous_timestamp_ms: int | None,
    ) -> tuple[float, int]:
        raw_timestamp_ms = float(capture.get(self._cv2.CAP_PROP_POS_MSEC))
        if not math.isfinite(raw_timestamp_ms) or raw_timestamp_ms < 0:
            message = (
                "OpenCV did not expose a finite nonnegative presentation timestamp "
                f"for decoded frame {frame_index}"
            )
            raise RuntimeError(message)
        timestamp_ms = round(raw_timestamp_ms)
        if previous_raw_timestamp_ms is not None and raw_timestamp_ms <= previous_raw_timestamp_ms:
            message = (
                "decoded presentation timestamps must increase; "
                f"frame {frame_index} reported {raw_timestamp_ms:g} ms after "
                f"{previous_raw_timestamp_ms:g} ms"
            )
            raise RuntimeError(message)
        if previous_timestamp_ms is not None and timestamp_ms <= previous_timestamp_ms:
            message = (
                "decoded presentation timestamps collapse at millisecond resolution; "
                f"frame {frame_index} rounded to {timestamp_ms} ms"
            )
            raise RuntimeError(message)
        return raw_timestamp_ms, timestamp_ms

    def _scan_duration_ms(self) -> int:
        """Scan PTS once so a VFR clip has an evidence-backed half-open duration."""
        capture = self._open_capture()
        previous_raw_timestamp_ms: float | None = None
        previous_timestamp_ms: int | None = None
        frame_index = 0
        try:
            while True:
                decoded, _pixels = capture.read()
                if not decoded:
                    break
                previous_raw_timestamp_ms, previous_timestamp_ms = self._decoded_timestamp(
                    capture,
                    frame_index,
                    previous_raw_timestamp_ms,
                    previous_timestamp_ms,
                )
                frame_index += 1
        finally:
            capture.release()
        if previous_raw_timestamp_ms is None:
            message = "input video did not decode any timestamped frames"
            raise RuntimeError(message)
        return math.floor(previous_raw_timestamp_ms) + 1

    def _rewind(self) -> None:
        """Reopen instead of timestamp-seeking, whose landing semantics vary by backend."""
        self._capture.release()
        self._capture = self._open_capture()
        self._previous_raw_timestamp_ms = None
        self._previous_timestamp_ms = None
        self._previous_requested_timestamp_ms = None

    @property
    def duration_ms(self) -> int:
        """Return one millisecond past the final decoded presentation timestamp."""
        return self._duration_ms

    def read_at(self, timestamp_ms: int) -> contract.VideoFrame | None:
        """Decode forward to the first source frame at or after the requested time."""
        if timestamp_ms < 0:
            message = "timestamp_ms must not be negative"
            raise ValueError(message)
        if self._closed:
            message = "video reader is closed"
            raise RuntimeError(message)
        if timestamp_ms >= self._duration_ms:
            return None
        if (
            self._previous_requested_timestamp_ms is not None
            and timestamp_ms <= self._previous_requested_timestamp_ms
        ):
            self._rewind()
        elif (
            self._previous_raw_timestamp_ms is not None
            and timestamp_ms <= self._previous_raw_timestamp_ms
        ):
            message = (
                "source cadence cannot provide distinct frames for consecutive sample "
                f"deadlines; {self._previous_raw_timestamp_ms:g} ms also satisfies "
                f"the {timestamp_ms} ms request"
            )
            raise RuntimeError(message)
        frame_index = 0
        while True:
            decoded, bgr_pixels = self._capture.read()
            if not decoded:
                return None
            raw_timestamp_ms, source_timestamp_ms = self._decoded_timestamp(
                self._capture,
                frame_index,
                self._previous_raw_timestamp_ms,
                self._previous_timestamp_ms,
            )
            self._previous_raw_timestamp_ms = raw_timestamp_ms
            self._previous_timestamp_ms = source_timestamp_ms
            frame_index += 1
            if raw_timestamp_ms < timestamp_ms:
                continue
            rgb_pixels = self._cv2.cvtColor(bgr_pixels, self._cv2.COLOR_BGR2RGB)
            height, width = rgb_pixels.shape[:2]
            self._previous_requested_timestamp_ms = timestamp_ms
            return contract.VideoFrame(
                pixels=rgb_pixels,
                width=int(width),
                height=int(height),
                source_timestamp_ms=source_timestamp_ms,
            )

    def close(self) -> None:
        """Release the native decoder."""
        if not self._closed:
            self._capture.release()
            self._closed = True

    def __enter__(self) -> Self:
        """Return the opened reader."""
        return self

    def __exit__(self, *_unused: object) -> None:
        """Release the reader at context exit."""
        self.close()


def _optional_float(value: float | None) -> float | None:
    if value is None:
        return None
    return value


def _convert_landmark(value: _NativeLandmark) -> contract.Landmark:
    return contract.Landmark(
        x=float(value.x),
        y=float(value.y),
        z=float(value.z),
        visibility=_optional_float(value.visibility),
        presence=_optional_float(value.presence),
    )


@final
class MediaPipePoseLandmarker:
    """Use the official MediaPipe Tasks VIDEO-mode API."""

    def __init__(
        self, mediapipe_module: _MediaPipeModule, model_path: Path, *, num_poses: int = 1
    ) -> None:
        """Construct a CPU pose task backed by an explicit model asset."""
        if num_poses <= 0:
            message = "num_poses must be positive"
            raise ValueError(message)
        self._mediapipe = mediapipe_module
        options = mediapipe_module.tasks.vision.PoseLandmarkerOptions(
            base_options=mediapipe_module.tasks.BaseOptions(model_asset_path=str(model_path)),
            running_mode=mediapipe_module.tasks.vision.RunningMode.VIDEO,
            num_poses=num_poses,
            output_segmentation_masks=False,
        )
        self._landmarker = mediapipe_module.tasks.vision.PoseLandmarker.create_from_options(options)

    def detect_for_video(self, rgb_pixels: object, timestamp_ms: int) -> Sequence[contract.Pose]:
        """Infer normalized and world landmarks at one scheduled timestamp."""
        image = self._mediapipe.Image(
            image_format=self._mediapipe.ImageFormat.SRGB,
            data=rgb_pixels,
        )
        result = self._landmarker.detect_for_video(image, timestamp_ms)
        normalized = result.pose_landmarks
        world = result.pose_world_landmarks
        if len(normalized) != len(world):
            message = "MediaPipe returned mismatched normalized and world pose counts"
            raise RuntimeError(message)
        return tuple(
            contract.Pose(
                normalized_landmarks=tuple(_convert_landmark(value) for value in pose),
                world_landmarks=tuple(_convert_landmark(value) for value in world[index]),
            )
            for index, pose in enumerate(normalized)
        )

    def close(self) -> None:
        """Release the native MediaPipe task."""
        self._landmarker.close()

    def __enter__(self) -> Self:
        """Return the opened task."""
        return self

    def __exit__(self, *_unused: object) -> None:
        """Release the task at context exit."""
        self.close()
