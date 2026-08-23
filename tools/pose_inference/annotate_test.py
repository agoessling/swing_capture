"""Tests for joining and selecting annotated pose-trigger evidence."""

from __future__ import annotations

import io
import json
import unittest

from tools.pose_inference import annotate

OBSERVATION_HEADER = (
    "timestamp_us,person_confidence,address_confidence,motion_magnitude,inside_hitting_region"
)


def _landmark(timestamp_ms: int) -> str:
    landmarks = [{"x": 0.5, "y": 0.5, "visibility": 0.9} for _index in range(33)]
    return json.dumps(
        {
            "source_timestamp_ms": timestamp_ms,
            "poses": [{"normalized_landmarks": landmarks}],
        }
    )


class AnnotateEvidenceTest(unittest.TestCase):
    """Exercise strict joins without invoking a native video codec."""

    def test_joins_one_to_one_and_selects_latest_available_sample(self) -> None:
        """Never render evidence from the future onto an earlier decoded frame."""
        landmarks = [_landmark(0), _landmark(200)]
        observations = io.StringIO(
            f"{OBSERVATION_HEADER}\n0,0.9,0.2,0.4,true\n200000,0.8,0.7,0.1,false\n"
        )
        evidence = annotate.load_evidence(landmarks, observations)
        self.assertEqual(2, len(evidence))
        self.assertEqual(0, annotate.sample_at(evidence, 199.9).timestamp_ms)
        self.assertEqual(200, annotate.sample_at(evidence, 200).timestamp_ms)
        self.assertEqual(33, len(evidence[0].landmarks))

    def test_rejects_missing_or_extra_samples(self) -> None:
        """Do not silently annotate a scalar row with unrelated landmarks."""
        observations = f"{OBSERVATION_HEADER}\n200000,0.8,0.7,0.1,true\n"
        with self.assertRaises(ValueError):
            annotate.load_evidence([_landmark(0)], io.StringIO(observations))
        with self.assertRaises(ValueError):
            annotate.sample_at((), 0)

    def test_parses_visible_hitting_region(self) -> None:
        """Use the same normalized rectangle in feature extraction and rendering."""
        region = annotate.parse_hitting_region("0.2,0.4,0.8,1")
        self.assertEqual(
            (0.2, 0.4, 0.8, 1.0), (region.left, region.top, region.right, region.bottom)
        )
        for invalid in ("0,0,1", "word,0,1,1", "-0.1,0,1,1", "0.5,0,0.5,1"):
            with self.assertRaises(ValueError):
                annotate.parse_hitting_region(invalid)

    def test_address_annotation_does_not_gate_on_legacy_roi(self) -> None:
        """Keep pre-arm review overlays consistent with the production controller."""
        evidence = annotate.Evidence(
            timestamp_ms=0,
            landmarks=(),
            person_confidence=0.9,
            address_confidence=0.9,
            motion_magnitude=0.1,
            inside_hitting_region=False,
        )

        self.assertTrue(annotate.is_address_qualifying(evidence))


if __name__ == "__main__":
    unittest.main()
