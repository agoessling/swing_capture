"""Hermetic tests for the pose-trigger corpus metadata contract."""

from __future__ import annotations

import dataclasses
import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, cast

from tools.pose_inference import corpus

if TYPE_CHECKING:
    from collections.abc import Callable


class CorpusTest(unittest.TestCase):
    """Exercise schema, semantic, split, and private-media validation."""

    def test_checked_in_manifest_is_valid_and_balanced(self) -> None:
        """Keep the human-reviewed seed labels machine-readable."""
        manifest = Path(__file__).with_name("corpus_manifest.json").read_text(encoding="utf-8")
        parsed = corpus.parse_manifest(manifest)
        summary = corpus.summarize(parsed)

        self.assertEqual(12, summary["clip_count"])
        self.assertEqual(3, cast("dict[str, int]", summary["by_view"])["dtl"])
        self.assertEqual(4, cast("dict[str, int]", summary["by_view"])["atl"])
        self.assertEqual(6, cast("dict[str, int]", summary["by_disposition"])["primary_positive"])
        self.assertEqual(2, cast("dict[str, int]", summary["by_split"])["development"])
        d03 = next(clip for clip in parsed.clips if clip.clip_id == "D03")
        self.assertEqual(19_000, d03.labels.preferred_arm_ms)
        self.assertEqual(
            IntervalTuple(12_000, 19_000), IntervalTuple.from_interval(d03.labels.must_not_arm[0])
        )

    def test_rejects_duplicate_ids_and_source_leakage(self) -> None:
        """Prevent accidental double-counting and cross-split source leakage."""
        value = self._minimal_manifest()
        clips = cast("list[dict[str, object]]", value["clips"])
        duplicate = cast("dict[str, object]", json.loads(json.dumps(clips[0])))
        duplicate["source_group"] = "second-source"
        clips.append(duplicate)
        self._expect_error(value, "clip ids must be unique")

        value = self._minimal_manifest()
        clips = cast("list[dict[str, object]]", value["clips"])
        second = cast("dict[str, object]", json.loads(json.dumps(clips[0])))
        second["id"] = "ABC02"
        second["split"] = "validation"
        clips.append(second)
        self._expect_error(value, "crosses corpus splits")

    def test_rejects_bad_timing_view_and_path(self) -> None:
        """Reject labels that would make aggregate pass/fail results misleading."""
        self._mutate_and_reject(
            lambda clip: cast("dict[str, object]", clip["labels"]).__setitem__(
                "preferred_arm_ms", 9_500
            ),
            "safe <= preferred < takeaway",
        )
        self._mutate_and_reject(
            lambda clip: clip.__setitem__("view", "unsupported_front"),
            "view and capture_role disagree",
        )
        self._mutate_and_reject(
            lambda clip: cast("dict[str, object]", clip["media"]).__setitem__(
                "path", "../outside.mp4"
            ),
            "safe relative path",
        )
        self._mutate_and_reject(
            lambda clip: cast("dict[str, object]", clip["labels"]).__setitem__(
                "must_not_arm",
                [
                    {"start_ms": 0, "end_ms": 4_000, "reason": "first"},
                    {"start_ms": 3_000, "end_ms": 5_000, "reason": "overlap"},
                ],
            ),
            "sorted and nonoverlapping",
        )

    def test_positive_clip_does_not_require_a_configured_roi(self) -> None:
        """Keep corpus qualification aligned with the ROI-free controller."""
        value = self._minimal_manifest()
        clip = cast("list[dict[str, object]]", value["clips"])[0]
        clip["hitting_region"] = None

        parsed = corpus.parse_manifest(json.dumps(value))

        self.assertIsNone(parsed.clips[0].hitting_region)
        self.assertEqual("0,0,1,1", corpus.FULL_FRAME_HITTING_REGION.encoded())

    def test_private_media_is_root_bounded_and_hash_checked(self) -> None:
        """Verify detached media without making it a source dependency."""
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            clips = root / "clips"
            clips.mkdir()
            media = clips / "ABC01.mp4"
            media.write_bytes(b"private evaluation bytes")
            expected = hashlib.sha256(media.read_bytes()).hexdigest()
            value = self._minimal_manifest()
            clip = cast("list[dict[str, object]]", value["clips"])[0]
            cast("dict[str, object]", clip["media"])["sha256"] = expected
            parsed = corpus.parse_manifest(json.dumps(value))
            results = corpus.validate_media(parsed, root)
            self.assertEqual(expected, results[0]["sha256"])

            media.write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                corpus.validate_media(parsed, root)

    def _mutate_and_reject(
        self, mutation: Callable[[dict[str, object]], None], message: str
    ) -> None:
        value = self._minimal_manifest()
        clip = cast("list[dict[str, object]]", value["clips"])[0]
        mutation(clip)
        self._expect_error(value, message)

    def _expect_error(self, value: dict[str, object], message: str) -> None:
        with self.assertRaisesRegex(ValueError, message):
            corpus.parse_manifest(json.dumps(value))

    @staticmethod
    def _minimal_manifest() -> dict[str, object]:
        return {
            "schema_version": 1,
            "sample_period_ms": 200,
            "clips": [
                {
                    "id": "ABC01",
                    "title": "Example",
                    "creator": "Creator",
                    "source_url": "https://example.test/video",
                    "source_group": "example-source",
                    "source_trim_ms": [1_000, 11_000],
                    "media": {"path": "clips/ABC01.mp4", "sha256": None},
                    "hitting_region": [0.2, 0.4, 0.8, 1.0],
                    "view": "dtl",
                    "capture_role": "down_the_line",
                    "disposition": "primary_positive",
                    "split": "development",
                    "labels": {
                        "safe_arm_start_ms": 2_000,
                        "preferred_arm_ms": 3_000,
                        "takeaway_ms": 7_000,
                        "impact_ms": 8_000,
                        "reference_arm_ms": None,
                        "must_not_arm": [{"start_ms": 0, "end_ms": 2_000, "reason": "upright"}],
                    },
                    "notes": "Test clip.",
                }
            ],
        }


@dataclasses.dataclass(frozen=True)
class IntervalTuple:
    """Small value adapter that keeps the test assertion readable."""

    start_ms: int
    end_ms: int

    @classmethod
    def from_interval(cls, interval: corpus.Interval) -> IntervalTuple:
        """Copy the interval endpoints while ignoring its explanatory text."""
        return cls(interval.start_ms, interval.end_ms)


if __name__ == "__main__":
    unittest.main()
