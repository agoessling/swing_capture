"""Hermetic tests for pose corpus batch planning and aggregation."""

from __future__ import annotations

import json
import unittest
from pathlib import Path
from typing import cast

from tools.pose_inference import corpus, corpus_evaluate


class CorpusEvaluateTest(unittest.TestCase):
    """Keep geometry exclusions and aggregate failures explicit."""

    def test_builds_only_supported_positive_jobs(self) -> None:
        """Never score unsupported camera views as valid station positives."""
        parsed = self._manifest()
        jobs = corpus_evaluate.build_jobs(parsed, Path("/media"), Path("/output"))
        self.assertEqual(
            ("IN01", "IN02", "C01", "D01", "D03", "D04", "EE01"),
            tuple(job.clip.clip_id for job in jobs),
        )
        region = jobs[0].clip.hitting_region
        self.assertIsNotNone(region)
        if region is None:
            self.fail("positive job omitted its hitting region")
        self.assertEqual("0.35,0.55,0.68,0.98", region.encoded())

    def test_aggregate_preserves_failures_and_exclusions(self) -> None:
        """Report challenge failures without dropping their diagnostic evidence."""
        parsed = self._manifest()
        results: list[dict[str, object]] = [
            {"id": "IN01", "replay": {"passed": True}},
            {"id": "D04", "replay": {"passed": False}},
        ]
        report = corpus_evaluate.aggregate(parsed, results, 800)
        self.assertEqual(2, report["evaluated_clip_count"])
        self.assertEqual(1, report["passed_clip_count"])
        self.assertEqual(1, report["failed_clip_count"])
        excluded = cast("list[dict[str, object]]", report["excluded"])
        self.assertIn("C02", {value["id"] for value in excluded})

    def test_replay_result_is_strict(self) -> None:
        """Reject malformed child-stage output before publishing an aggregate."""
        valid = {
            "schema_version": 1,
            "observation_count": 10,
            "arm_request_ns": "1400000000",
            "arm_offset_from_preferred_ns": "-200000000",
            "high_speed_ready_ns": "2200000000",
            "ready_lead_before_takeaway_ns": "800000000",
            "armed_before_safe_window": False,
            "armed_in_forbidden_interval": False,
            "ready_by_takeaway": True,
            "passed": True,
            "outcome": "acceptable_early",
            "arm_request_count": 1,
            "final_state": "arm_requested",
        }
        self.assertTrue(corpus_evaluate.parse_replay_result(json.dumps(valid), "A")["passed"])
        invalid = dict(valid)
        invalid["arm_request_count"] = 0
        with self.assertRaises(ValueError):
            corpus_evaluate.parse_replay_result(json.dumps(invalid), "A")
        invalid = dict(valid)
        invalid["outcome"] = "forbidden_arm"
        with self.assertRaises(ValueError):
            corpus_evaluate.parse_replay_result(json.dumps(invalid), "A")
        invalid = dict(valid)
        invalid["unexpected"] = True
        with self.assertRaises(ValueError):
            corpus_evaluate.parse_replay_result(json.dumps(invalid), "A")
        with self.assertRaises(ValueError):
            corpus_evaluate.parse_replay_result('{"schema_version":2,"passed":true}', "A")
        with self.assertRaises(ValueError):
            corpus_evaluate.parse_replay_result('{"schema_version":1}', "A")

    @staticmethod
    def _manifest() -> corpus.Corpus:
        return corpus.parse_manifest(
            Path(__file__).with_name("corpus_manifest.json").read_text(encoding="utf-8")
        )


if __name__ == "__main__":
    unittest.main()
