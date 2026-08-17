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

    def set(self, property_id: int, value: float) -> bool: ...

    def read(self) -> tuple[bool, object]: ...

    def release(self) -> None: ...


class _Cv2Module(Protocol):
    CAP_PROP_FRAME_COUNT: int
    CAP_PROP_FPS: int
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
    """Seek constant-frame-rate prototype media through OpenCV."""

    def __init__(self, cv2_module: _Cv2Module, input_path: Path) -> None:
        """Open one video and derive a deterministic half-open duration."""
        self._cv2 = cv2_module
        self._capture = cv2_module.VideoCapture(str(input_path))
        if not self._capture.isOpened():
            message = f"cannot open input video: {input_path}"
            raise RuntimeError(message)
        frame_count = float(self._capture.get(cv2_module.CAP_PROP_FRAME_COUNT))
        frames_per_second = float(self._capture.get(cv2_module.CAP_PROP_FPS))
        if not math.isfinite(frame_count) or frame_count < 1:
            self.close()
            message = "input video does not report a positive frame count"
            raise RuntimeError(message)
        if not math.isfinite(frames_per_second) or frames_per_second <= 0:
            self.close()
            message = "input video does not report a positive frame rate"
            raise RuntimeError(message)
        # Sampling asks for the frame at or immediately following a timestamp. The
        # nominal container duration includes the final frame's display interval,
        # where no following frame exists. Bound the half-open schedule one
        # millisecond after the final CFR frame timestamp instead.
        self._duration_ms = math.floor(1000.0 * (frame_count - 1.0) / frames_per_second) + 1

    @property
    def duration_ms(self) -> int:
        """Return the duration derived from the container frame count and rate."""
        return self._duration_ms

    def read_at(self, timestamp_ms: int) -> contract.VideoFrame | None:
        """Seek to and decode the sample nearest the requested media time."""
        if timestamp_ms < 0:
            message = "timestamp_ms must not be negative"
            raise ValueError(message)
        if not self._capture.set(self._cv2.CAP_PROP_POS_MSEC, float(timestamp_ms)):
            return None
        decoded, bgr_pixels = self._capture.read()
        if not decoded:
            return None
        rgb_pixels = self._cv2.cvtColor(bgr_pixels, self._cv2.COLOR_BGR2RGB)
        height, width = rgb_pixels.shape[:2]
        source_timestamp_ms = round(float(self._capture.get(self._cv2.CAP_PROP_POS_MSEC)))
        return contract.VideoFrame(
            pixels=rgb_pixels,
            width=int(width),
            height=int(height),
            source_timestamp_ms=source_timestamp_ms,
        )

    def close(self) -> None:
        """Release the native decoder."""
        self._capture.release()

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
