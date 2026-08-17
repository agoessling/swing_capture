"""Convert versioned MediaPipe NDJSON into the Java landmark replay contract."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import TYPE_CHECKING, cast

from tools.pose_inference.contract import SCHEMA_VERSION

if TYPE_CHECKING:
    from collections.abc import Iterable, Sequence
    from typing import TextIO


JOINT_INDICES = (
    ("left_shoulder", 11),
    ("right_shoulder", 12),
    ("left_elbow", 13),
    ("right_elbow", 14),
    ("left_wrist", 15),
    ("right_wrist", 16),
    ("left_hip", 23),
    ("right_hip", 24),
    ("left_knee", 25),
    ("right_knee", 26),
    ("left_ankle", 27),
    ("right_ankle", 28),
)
LANDMARK_COUNT = 33

HEADER = (
    "timestamp_us",
    "detector_person_confidence",
    *(
        field
        for joint, _index in JOINT_INDICES
        for field in (f"{joint}_x", f"{joint}_y", f"{joint}_visibility")
    ),
)


def _usable_landmark(value: object) -> tuple[str, str, str] | None:
    if not isinstance(value, dict):
        return None
    encoded = cast("dict[str, object]", value)
    raw_x = encoded.get("x")
    raw_y = encoded.get("y")
    raw_visibility = encoded.get("visibility")
    if not all(
        isinstance(field, (int, float)) and not isinstance(field, bool)
        for field in (raw_x, raw_y, raw_visibility)
    ):
        return None
    x = float(cast("int | float", raw_x))
    y = float(cast("int | float", raw_y))
    visibility = float(cast("int | float", raw_visibility))
    if not (0.0 <= x <= 1.0 and 0.0 <= y <= 1.0 and 0.0 <= visibility <= 1.0):
        return None
    return (format(x, ".17g"), format(y, ".17g"), format(visibility, ".17g"))


def _parse_record(line: str, line_number: int) -> tuple[int, list[object]]:
    if not line.strip():
        message = f"blank inference record at line {line_number}"
        raise ValueError(message)
    value = cast("dict[str, object]", json.loads(line))
    if value.get("schema_version") != SCHEMA_VERSION:
        message = f"unsupported inference schema at line {line_number}"
        raise ValueError(message)
    source_timestamp_ms = value.get("source_timestamp_ms")
    if not isinstance(source_timestamp_ms, int) or isinstance(source_timestamp_ms, bool):
        message = f"invalid source timestamp at line {line_number}"
        raise TypeError(message)
    poses = value.get("poses")
    if not isinstance(poses, list):
        message = f"expected zero or one pose at line {line_number}"
        raise TypeError(message)
    typed_poses = cast("list[object]", poses)
    if len(typed_poses) > 1:
        message = f"expected zero or one pose at line {line_number}"
        raise ValueError(message)
    if not typed_poses:
        return source_timestamp_ms * 1_000, []
    pose = typed_poses[0]
    if not isinstance(pose, dict):
        message = f"invalid pose at line {line_number}"
        raise TypeError(message)
    encoded_pose = cast("dict[str, object]", pose)
    candidate = encoded_pose.get("normalized_landmarks")
    if not isinstance(candidate, list):
        message = f"expected {LANDMARK_COUNT} normalized landmarks at line {line_number}"
        raise TypeError(message)
    landmarks = cast("list[object]", candidate)
    if len(landmarks) != LANDMARK_COUNT:
        message = f"expected {LANDMARK_COUNT} normalized landmarks at line {line_number}"
        raise ValueError(message)
    return source_timestamp_ms * 1_000, landmarks


def _row(timestamp_us: int, landmarks: list[object]) -> list[str | int]:
    row: list[str | int] = [timestamp_us, "1" if landmarks else "0"]
    for _joint, index in JOINT_INDICES:
        fields = _usable_landmark(landmarks[index]) if landmarks else None
        row.extend(fields if fields is not None else ("", "", ""))
    return row


def convert(lines: Iterable[str], output: TextIO) -> int:
    """Validate inference records and emit one fixed-width landmark CSV row each."""
    writer = csv.writer(output, lineterminator="\n")
    writer.writerow(HEADER)
    previous_timestamp_us = -1
    count = 0
    for line_number, line in enumerate(lines, start=1):
        timestamp_us, landmarks = _parse_record(line, line_number)
        if timestamp_us <= previous_timestamp_us:
            message = f"timestamps must increase at line {line_number}"
            raise ValueError(message)
        previous_timestamp_us = timestamp_us
        writer.writerow(_row(timestamp_us, landmarks))
        count += 1
    if count == 0:
        message = "inference input must contain at least one record"
        raise ValueError(message)
    return count


def main(arguments: Sequence[str] | None = None) -> int:
    """Convert one inference result file."""
    parser = argparse.ArgumentParser(description="convert pose NDJSON to fixed landmark CSV")
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parsed = parser.parse_args(arguments)
    input_path = cast("Path", parsed.input)
    output_path = cast("Path", parsed.output)
    with (
        input_path.open("r", encoding="utf-8") as source,
        output_path.open("w", encoding="utf-8", newline="") as output,
    ):
        count = convert(source, output)
    print(f"emitted {count} landmark rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
