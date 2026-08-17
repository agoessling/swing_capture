"""Dependency-free contracts for timestamped host pose inference."""

from __future__ import annotations

import dataclasses
import json
from typing import TYPE_CHECKING, Protocol

if TYPE_CHECKING:
    from collections.abc import Sequence
    from typing import TextIO


SCHEMA_VERSION = 1
DEFAULT_SAMPLE_PERIOD_MS = 200


@dataclasses.dataclass(frozen=True)
class Landmark:
    """One MediaPipe-compatible 3D landmark."""

    x: float
    y: float
    z: float
    visibility: float | None = None
    presence: float | None = None

    def as_json(self) -> dict[str, float | None]:
        """Return the stable serialized representation."""
        return dataclasses.asdict(self)


@dataclasses.dataclass(frozen=True)
class Pose:
    """Normalized and metric landmarks for one detected person."""

    normalized_landmarks: tuple[Landmark, ...]
    world_landmarks: tuple[Landmark, ...]

    def as_json(self) -> dict[str, object]:
        """Return the stable serialized representation."""
        return {
            "normalized_landmarks": [value.as_json() for value in self.normalized_landmarks],
            "world_landmarks": [value.as_json() for value in self.world_landmarks],
        }


@dataclasses.dataclass(frozen=True)
class VideoFrame:
    """One decoded RGB frame and its observed source timestamp."""

    pixels: object = dataclasses.field(repr=False, compare=False)
    width: int
    height: int
    source_timestamp_ms: int


@dataclasses.dataclass(frozen=True)
class FrameResult:
    """All pose detections produced for one scheduled sample."""

    sample_index: int
    timestamp_ms: int
    source_timestamp_ms: int
    width: int
    height: int
    poses: tuple[Pose, ...]

    def as_json(self) -> dict[str, object]:
        """Return one self-describing NDJSON record."""
        return {
            "schema_version": SCHEMA_VERSION,
            "sample_index": self.sample_index,
            "timestamp_ms": self.timestamp_ms,
            "source_timestamp_ms": self.source_timestamp_ms,
            "image": {"width": self.width, "height": self.height},
            "poses": [pose.as_json() for pose in self.poses],
        }


class VideoReader(Protocol):
    """Seekable decoded-video boundary used by the deterministic coordinator."""

    @property
    def duration_ms(self) -> int:
        """Return the half-open media duration in milliseconds."""
        ...

    def read_at(self, timestamp_ms: int) -> VideoFrame | None:
        """Decode the frame at or immediately following the requested timestamp."""
        ...


class PoseLandmarker(Protocol):
    """Timestamp-aware pose model boundary."""

    def detect_for_video(self, rgb_pixels: object, timestamp_ms: int) -> Sequence[Pose]:
        """Infer poses for an RGB frame using a monotonically increasing timestamp."""
        ...


def sample_timestamps(
    duration_ms: int, sample_period_ms: int = DEFAULT_SAMPLE_PERIOD_MS
) -> tuple[int, ...]:
    """Return exact half-open sample timestamps without floating-point drift."""
    if duration_ms < 0:
        message = "duration_ms must not be negative"
        raise ValueError(message)
    if sample_period_ms <= 0:
        message = "sample_period_ms must be positive"
        raise ValueError(message)
    return tuple(range(0, duration_ms, sample_period_ms))


def run_inference(
    reader: VideoReader,
    landmarker: PoseLandmarker,
    output: TextIO,
    *,
    sample_period_ms: int = DEFAULT_SAMPLE_PERIOD_MS,
) -> int:
    """Sample a video, infer poses, and emit deterministic newline-delimited JSON."""
    timestamps = sample_timestamps(reader.duration_ms, sample_period_ms)
    for sample_index, timestamp_ms in enumerate(timestamps):
        frame = reader.read_at(timestamp_ms)
        if frame is None:
            message = f"video decoder returned no frame for {timestamp_ms} ms"
            raise RuntimeError(message)
        if frame.width <= 0 or frame.height <= 0:
            message = f"video decoder returned invalid geometry at {timestamp_ms} ms"
            raise RuntimeError(message)
        poses = tuple(landmarker.detect_for_video(frame.pixels, timestamp_ms))
        value = FrameResult(
            sample_index=sample_index,
            timestamp_ms=timestamp_ms,
            source_timestamp_ms=frame.source_timestamp_ms,
            width=frame.width,
            height=frame.height,
            poses=poses,
        )
        json.dump(value.as_json(), output, separators=(",", ":"), sort_keys=True)
        output.write("\n")
    return len(timestamps)
