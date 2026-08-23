"""Render auditable pose landmarks, features, and high-speed arm timing onto a clip."""

from __future__ import annotations

import argparse
import bisect
import csv
import dataclasses
import importlib
import json
from pathlib import Path
from typing import TYPE_CHECKING, Protocol, cast

from tools.pose_inference.corpus import FULL_FRAME_HITTING_REGION, HittingRegion

if TYPE_CHECKING:
    from collections.abc import Iterable, Sequence


SKELETON_EDGES = (
    (11, 12),
    (11, 13),
    (13, 15),
    (12, 14),
    (14, 16),
    (11, 23),
    (12, 24),
    (23, 24),
    (23, 25),
    (25, 27),
    (24, 26),
    (26, 28),
)
POSE_LANDMARK_COUNT = 33
MINIMUM_VISIBLE_LANDMARK = 0.5
MINIMUM_PERSON_CONFIDENCE = 0.55
MINIMUM_ADDRESS_CONFIDENCE = 0.45
MAXIMUM_ADDRESS_MOTION = 0.45
HITTING_REGION_FIELD_COUNT = 4


class _Capture(Protocol):
    def isOpened(self) -> bool: ...  # noqa: N802

    def get(self, property_id: int) -> float: ...

    def read(self) -> tuple[bool, object]: ...

    def release(self) -> None: ...


class _Writer(Protocol):
    def isOpened(self) -> bool: ...  # noqa: N802

    def write(self, frame: object) -> None: ...

    def release(self) -> None: ...


class _Cv2(Protocol):
    CAP_PROP_FRAME_WIDTH: int
    CAP_PROP_FRAME_HEIGHT: int
    CAP_PROP_FPS: int
    FONT_HERSHEY_SIMPLEX: int
    LINE_AA: int

    def VideoCapture(self, path: str) -> _Capture: ...  # noqa: N802

    def VideoWriter_fourcc(self, *characters: str) -> int: ...  # noqa: N802

    def VideoWriter(  # noqa: N802
        self, path: str, fourcc: int, frames_per_second: float, size: tuple[int, int]
    ) -> _Writer: ...

    def rectangle(
        self,
        image: object,
        first: tuple[int, int],
        second: tuple[int, int],
        color: tuple[int, int, int],
        thickness: int,
    ) -> object: ...

    def circle(
        self,
        image: object,
        center: tuple[int, int],
        radius: int,
        color: tuple[int, int, int],
        thickness: int,
    ) -> object: ...

    def line(  # noqa: PLR0913
        self,
        image: object,
        first: tuple[int, int],
        second: tuple[int, int],
        color: tuple[int, int, int],
        thickness: int,
        line_type: int,
    ) -> object: ...

    def putText(  # noqa: N802, PLR0913
        self,
        image: object,
        text: str,
        origin: tuple[int, int],
        font: int,
        scale: float,
        color: tuple[int, int, int],
        thickness: int,
        line_type: int,
    ) -> object: ...


@dataclasses.dataclass(frozen=True)
class Evidence:
    """One joined inference and shared-Java feature sample."""

    timestamp_ms: int
    landmarks: tuple[tuple[float, float, float] | None, ...]
    person_confidence: float
    address_confidence: float
    motion_magnitude: float
    inside_hitting_region: bool


def _landmark_samples(
    lines: Iterable[str],
) -> dict[int, tuple[tuple[float, float, float] | None, ...]]:
    samples: dict[int, tuple[tuple[float, float, float] | None, ...]] = {}
    for line in lines:
        decoded = cast("object", json.loads(line))
        if not isinstance(decoded, dict):
            message = "landmark record must be an object"
            raise TypeError(message)
        value = cast("dict[str, object]", decoded)
        timestamp = value.get("source_timestamp_ms")
        poses = value.get("poses")
        if not isinstance(timestamp, int) or isinstance(timestamp, bool):
            message = "invalid landmark timestamp"
            raise TypeError(message)
        if not isinstance(poses, list):
            message = "expected zero or one pose"
            raise TypeError(message)
        typed_poses = cast("list[object]", poses)
        if len(typed_poses) > 1:
            message = "expected zero or one pose"
            raise ValueError(message)
        converted = _convert_landmarks(typed_poses)
        if converted and len(converted) != POSE_LANDMARK_COUNT:
            message = "expected 33 landmarks"
            raise ValueError(message)
        if timestamp in samples:
            message = "duplicate landmark timestamp"
            raise ValueError(message)
        samples[timestamp] = tuple(converted)
    return samples


def _convert_landmarks(
    poses: list[object],
) -> list[tuple[float, float, float] | None]:
    if not poses:
        return []
    pose = poses[0]
    if not isinstance(pose, dict):
        message = "invalid normalized landmarks"
        raise TypeError(message)
    encoded_pose = cast("dict[str, object]", pose)
    normalized_landmarks = encoded_pose.get("normalized_landmarks")
    if not isinstance(normalized_landmarks, list):
        message = "invalid normalized landmarks"
        raise TypeError(message)
    converted: list[tuple[float, float, float] | None] = []
    for raw in cast("list[object]", normalized_landmarks):
        if not isinstance(raw, dict):
            converted.append(None)
            continue
        encoded = cast("dict[str, object]", raw)
        raw_x = encoded.get("x")
        raw_y = encoded.get("y")
        raw_visibility = encoded.get("visibility")
        if not all(
            isinstance(field, (int, float)) and not isinstance(field, bool)
            for field in (raw_x, raw_y, raw_visibility)
        ):
            converted.append(None)
            continue
        converted.append(
            (
                float(cast("int | float", raw_x)),
                float(cast("int | float", raw_y)),
                float(cast("int | float", raw_visibility)),
            )
        )
    return converted


def load_evidence(
    landmark_lines: Iterable[str], observation_lines: Iterable[str]
) -> tuple[Evidence, ...]:
    """Join raw landmarks to the exact scalar observations produced by Java."""
    landmarks = _landmark_samples(landmark_lines)
    reader = csv.DictReader(observation_lines)
    expected = {
        "timestamp_us",
        "person_confidence",
        "address_confidence",
        "motion_magnitude",
        "inside_hitting_region",
    }
    if reader.fieldnames is None or set(reader.fieldnames) != expected:
        message = "unexpected observation CSV header"
        raise ValueError(message)
    evidence: list[Evidence] = []
    for row in reader:
        timestamp_us = int(row["timestamp_us"])
        if timestamp_us % 1_000 != 0:
            message = "observation timestamp is not millisecond-aligned"
            raise ValueError(message)
        timestamp_ms = timestamp_us // 1_000
        if evidence and timestamp_ms <= evidence[-1].timestamp_ms:
            message = "observation timestamps must increase"
            raise ValueError(message)
        if timestamp_ms not in landmarks:
            message = "observation has no matching landmark sample"
            raise ValueError(message)
        inside = row["inside_hitting_region"]
        if inside not in {"true", "false"}:
            message = "invalid hitting-region boolean"
            raise ValueError(message)
        evidence.append(
            Evidence(
                timestamp_ms=timestamp_ms,
                landmarks=landmarks.pop(timestamp_ms),
                person_confidence=float(row["person_confidence"]),
                address_confidence=float(row["address_confidence"]),
                motion_magnitude=float(row["motion_magnitude"]),
                inside_hitting_region=inside == "true",
            )
        )
    if not evidence or landmarks:
        message = "landmark and observation samples are not one-to-one"
        raise ValueError(message)
    return tuple(evidence)


def sample_at(evidence: Sequence[Evidence], timestamp_ms: float) -> Evidence:
    """Return the latest 5 fps evidence available at a decoded frame time."""
    if not evidence:
        message = "evidence cannot be empty"
        raise ValueError(message)
    timestamps = [sample.timestamp_ms for sample in evidence]
    index = max(0, bisect.bisect_right(timestamps, timestamp_ms) - 1)
    return evidence[index]


def _point(
    landmark: tuple[float, float, float] | None, width: int, height: int
) -> tuple[int, int] | None:
    if landmark is None or landmark[2] < MINIMUM_VISIBLE_LANDMARK:
        return None
    x, y, _visibility = landmark
    if not (0.0 <= x <= 1.0 and 0.0 <= y <= 1.0):
        return None
    return round(x * (width - 1)), round(y * (height - 1))


def is_address_qualifying(evidence: Evidence) -> bool:
    """Mirror the ROI-free controller's per-observation qualification predicate."""
    return (
        evidence.person_confidence >= MINIMUM_PERSON_CONFIDENCE
        and evidence.address_confidence >= MINIMUM_ADDRESS_CONFIDENCE
        and evidence.motion_magnitude <= MAXIMUM_ADDRESS_MOTION
    )


def _draw(  # noqa: C901, PLR0913
    cv2: _Cv2,
    frame: object,
    evidence: Evidence,
    width: int,
    height: int,
    timestamp_ms: float,
    arm_ms: int,
    ready_ms: int,
    takeaway_ms: int,
    hitting_region: HittingRegion,
) -> None:
    if hitting_region != FULL_FRAME_HITTING_REGION:
        cv2.rectangle(
            frame,
            (round(hitting_region.left * (width - 1)), round(hitting_region.top * (height - 1))),
            (
                round(hitting_region.right * (width - 1)),
                round(hitting_region.bottom * (height - 1)),
            ),
            (255, 170, 0),
            3,
        )
    points = tuple(_point(value, width, height) for value in evidence.landmarks)
    for first, second in SKELETON_EDGES:
        if first < len(points) and second < len(points):
            first_point = points[first]
            second_point = points[second]
            if first_point is not None and second_point is not None:
                cv2.line(frame, first_point, second_point, (0, 255, 255), 2, cv2.LINE_AA)
    for point in points:
        if point is not None:
            cv2.circle(frame, point, 4, (0, 220, 0), -1)

    qualifies = is_address_qualifying(evidence)
    if timestamp_ms >= ready_ms:
        status, status_color = "HIGH SPEED READY", (40, 220, 40)
    elif timestamp_ms >= arm_ms:
        status, status_color = "START HIGH SPEED", (0, 180, 255)
    elif qualifies:
        status, status_color = "ADDRESS QUALIFYING", (0, 220, 255)
    else:
        status, status_color = "LOW-RATE WATCH", (230, 230, 230)
    cv2.rectangle(frame, (8, 8), (min(width - 8, 690), 146), (10, 10, 10), -1)
    lines = (
        f"{timestamp_ms / 1000.0:5.2f}s  {status}",
        f"person {evidence.person_confidence:.2f}  address {evidence.address_confidence:.2f}",
        (
            f"motion {evidence.motion_magnitude:.2f}  ROI diagnostic "
            f"{str(evidence.inside_hitting_region).lower()}"
        ),
        (
            f"arm {arm_ms / 1000.0:.2f}s  ready {ready_ms / 1000.0:.2f}s  "
            f"takeaway {takeaway_ms / 1000.0:.2f}s"
        ),
    )
    for index, text in enumerate(lines):
        color = status_color if index == 0 else (235, 235, 235)
        cv2.putText(
            frame,
            text,
            (20, 38 + index * 31),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.68,
            color,
            2,
            cv2.LINE_AA,
        )


def render(  # noqa: PLR0913
    cv2: _Cv2,
    input_path: Path,
    output_path: Path,
    evidence: Sequence[Evidence],
    arm_ms: int,
    startup_budget_ms: int,
    takeaway_ms: int,
    hitting_region: HittingRegion,
) -> int:
    """Decode sequentially and write a review MP4 with evidence overlays."""
    capture = cv2.VideoCapture(str(input_path))
    if not capture.isOpened():
        message = f"cannot open input video: {input_path}"
        raise RuntimeError(message)
    width = round(capture.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = round(capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
    frames_per_second = float(capture.get(cv2.CAP_PROP_FPS))
    if width <= 0 or height <= 0 or frames_per_second <= 0:
        capture.release()
        message = "input video reports invalid geometry or frame rate"
        raise RuntimeError(message)
    writer = cv2.VideoWriter(
        str(output_path),
        cv2.VideoWriter_fourcc(*"mp4v"),
        frames_per_second,
        (width, height),
    )
    if not writer.isOpened():
        capture.release()
        message = f"cannot open output video: {output_path}"
        raise RuntimeError(message)
    frame_index = 0
    try:
        while True:
            decoded, frame = capture.read()
            if not decoded:
                break
            timestamp_ms = 1_000.0 * frame_index / frames_per_second
            _draw(
                cv2,
                frame,
                sample_at(evidence, timestamp_ms),
                width,
                height,
                timestamp_ms,
                arm_ms,
                arm_ms + startup_budget_ms,
                takeaway_ms,
                hitting_region,
            )
            writer.write(frame)
            frame_index += 1
    finally:
        capture.release()
        writer.release()
    if frame_index == 0:
        message = "input video decoded no frames"
        raise RuntimeError(message)
    return frame_index


def parse_hitting_region(encoded: str) -> HittingRegion:
    """Parse a normalized left,top,right,bottom CLI value."""
    fields = encoded.split(",")
    if len(fields) != HITTING_REGION_FIELD_COUNT:
        message = "hitting-region must use left,top,right,bottom"
        raise ValueError(message)
    try:
        values = tuple(float(value) for value in fields)
    except ValueError as exception:
        message = "hitting-region contains a nonnumeric coordinate"
        raise ValueError(message) from exception
    if any(value < 0.0 or value > 1.0 for value in values):
        message = "hitting-region coordinates must be in [0, 1]"
        raise ValueError(message)
    left, top, right, bottom = values
    if right <= left or bottom <= top:
        message = "hitting-region must have positive width and height"
        raise ValueError(message)
    return HittingRegion(left, top, right, bottom)


def main(arguments: Sequence[str] | None = None) -> int:
    """Render one annotated pose-trigger review clip."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--landmarks", type=Path, required=True)
    parser.add_argument("--observations", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--arm-ms", type=int, required=True)
    parser.add_argument("--startup-budget-ms", type=int, required=True)
    parser.add_argument("--takeaway-ms", type=int, required=True)
    parser.add_argument("--hitting-region", required=True)
    options = parser.parse_args(arguments)
    input_path = cast("Path", options.input)
    landmarks_path = cast("Path", options.landmarks)
    observations_path = cast("Path", options.observations)
    output_path = cast("Path", options.output)
    arm_ms = cast("int", options.arm_ms)
    startup_budget_ms = cast("int", options.startup_budget_ms)
    takeaway_ms = cast("int", options.takeaway_ms)
    hitting_region = cast("str", options.hitting_region)
    cv2 = cast("_Cv2", cast("object", importlib.import_module("cv2")))
    with (
        landmarks_path.open("r", encoding="utf-8") as landmark_input,
        observations_path.open("r", encoding="utf-8") as observation_input,
    ):
        evidence = load_evidence(landmark_input, observation_input)
    count = render(
        cv2,
        input_path,
        output_path,
        evidence,
        arm_ms,
        startup_budget_ms,
        takeaway_ms,
        parse_hitting_region(hitting_region),
    )
    print(f"rendered {count} annotated frames")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
