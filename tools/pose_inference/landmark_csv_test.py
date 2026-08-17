"""Tests for the strict MediaPipe-to-Java landmark CSV adapter."""

from __future__ import annotations

import io
import json
import unittest

from tools.pose_inference import landmark_csv


def _record(timestamp_ms: int, poses: list[dict[str, object]]) -> str:
    return json.dumps(
        {
            "schema_version": 1,
            "source_timestamp_ms": timestamp_ms,
            "poses": poses,
        }
    )


class LandmarkCsvTest(unittest.TestCase):
    """Exercise strict conversion of raw MediaPipe records."""

    def test_maps_media_pipe_indices_and_empty_frames(self) -> None:
        """Map the twelve shared joints and preserve explicit no-pose samples."""
        landmarks = [
            {"x": index / 100.0, "y": (index + 1) / 100.0, "visibility": 0.75}
            for index in range(33)
        ]
        output = io.StringIO()
        count = landmark_csv.convert(
            [_record(0, [{"normalized_landmarks": landmarks}]), _record(200, [])], output
        )
        rows = output.getvalue().splitlines()
        self.assertEqual(2, count)
        self.assertEqual(landmark_csv.HEADER, tuple(rows[0].split(",")))
        self.assertEqual(["0", "1", "0.11", "0.12", "0.75"], rows[1].split(",")[:5])
        self.assertEqual("200000,0," + "," * 35, rows[2])

    def test_omits_out_of_frame_or_invalid_landmarks(self) -> None:
        """Do not clamp extrapolated model coordinates into a false in-frame pose."""
        landmarks: list[object] = [{"x": 0.5, "y": 0.5, "visibility": 0.8} for _index in range(33)]
        landmarks[11] = {"x": -0.1, "y": 0.5, "visibility": 0.9}
        output = io.StringIO()
        landmark_csv.convert([_record(0, [{"normalized_landmarks": landmarks}])], output)
        self.assertEqual(["", "", ""], output.getvalue().splitlines()[1].split(",")[2:5])

    def test_rejects_bad_schema_shape_and_timestamps(self) -> None:
        """Reject evidence that cannot be mapped without ambiguity."""
        valid_pose: dict[str, object] = {
            "normalized_landmarks": [dict[str, object]() for _index in range(33)]
        }
        for lines in (
            [""],
            [json.dumps({"schema_version": 2, "source_timestamp_ms": 0, "poses": []})],
            [_record(0, [valid_pose, valid_pose])],
            [_record(0, []), _record(0, [])],
            [_record(0, [{"normalized_landmarks": []}])],
        ):
            with self.assertRaises(ValueError):
                landmark_csv.convert(lines, io.StringIO())


if __name__ == "__main__":
    unittest.main()
