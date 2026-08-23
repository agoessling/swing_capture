"""Deterministic tests for same-holdout production comparison enforcement."""

from __future__ import annotations

import copy
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import cast

from python.runfiles import runfiles

from tools.field_evidence import audio_holdout_compare


def _runfile(logical_path: str) -> Path:
    resolver = runfiles.Create()
    if resolver is None:
        message = "Bazel runfiles resolver is unavailable"
        raise RuntimeError(message)
    resolved = resolver.Rlocation(f"_main/{logical_path}")
    if resolved is None:
        message = f"runfile is unavailable: {logical_path}"
        raise RuntimeError(message)
    return Path(resolved)


class AudioHoldoutCompareTest(unittest.TestCase):
    """Exercise the holdout comparator's policy and executable contract."""

    def test_candidate_must_pass_improve_and_not_regress(self) -> None:
        """Require strict improvement without allowing baseline regressions."""
        baseline = self._report(production=True)
        candidate = self._report(production=False)
        cast("dict[str, object]", candidate["candidate_generation"])[
            "non_target_candidate_count"
        ] = 1
        report = audio_holdout_compare.compare_reports(baseline, candidate)
        self.assertTrue(report["passed"])
        comparisons = cast("dict[str, dict[str, object]]", report["comparisons"])
        self.assertTrue(comparisons["non_target_candidate_count"]["strict_improvement"])

        equal = self._report(production=False)
        self.assertFalse(audio_holdout_compare.compare_reports(baseline, equal)["passed"])

        regressed = copy.deepcopy(candidate)
        cast("dict[str, object]", regressed["lifecycle"])["high_speed_duty_ppm"] = 200_001
        regression = audio_holdout_compare.compare_reports(baseline, regressed)
        self.assertFalse(regression["passed"])
        self.assertFalse(cast("dict[str, object]", regression["checks"])["no_baseline_regressions"])

    def test_comparison_rejects_different_holdout_and_nonproduction_baseline(self) -> None:
        """Reject incomparable evidence and a nonproduction baseline."""
        baseline = self._report(production=True)
        candidate = self._report(production=False)
        candidate["holdout_recording_ids"] = ["other"]
        with self.assertRaisesRegex(ValueError, "holdout recording IDs differ"):
            audio_holdout_compare.compare_reports(baseline, candidate)

        baseline = self._report(production=False)
        candidate = self._report(production=False)
        cast("dict[str, object]", candidate["detector"])["algorithm_id"] = "third-detector"
        with self.assertRaisesRegex(ValueError, "baseline is not the production"):
            audio_holdout_compare.compare_reports(baseline, candidate)

    def test_comparison_rejects_unversioned_or_different_matching_semantics(self) -> None:
        """Reject reports whose candidate matching semantics are incompatible."""
        baseline = self._report(production=True)
        candidate = self._report(production=False)
        cast("dict[str, object]", baseline["evaluation_policy"]).pop(
            "continuous_candidate_matching"
        )
        with self.assertRaisesRegex(ValueError, "unsupported continuous-candidate matching"):
            audio_holdout_compare.compare_reports(baseline, candidate)

        baseline = self._report(production=True)
        cast("dict[str, object]", candidate["evaluation_policy"])[
            "continuous_candidate_matching"
        ] = "many_to_many_legacy"
        with self.assertRaisesRegex(ValueError, "unsupported continuous-candidate matching"):
            audio_holdout_compare.compare_reports(baseline, candidate)

    def test_executable_returns_one_and_preserves_failed_comparison(self) -> None:
        """Persist a failed report and refuse to overwrite existing evidence."""
        executable = _runfile("tools/field_evidence/audio_holdout_compare")
        baseline = self._report(production=True)
        candidate = self._report(production=False)
        cast("dict[str, object]", candidate["lifecycle"])["high_speed_duty_ppm"] = 200_001
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            baseline_path = root / "baseline.json"
            candidate_path = root / "candidate.json"
            output = root / "comparison.json"
            baseline_path.write_text(json.dumps(baseline), encoding="utf-8")
            candidate_path.write_text(json.dumps(candidate), encoding="utf-8")
            completed = subprocess.run(
                [
                    str(executable),
                    "--baseline",
                    str(baseline_path),
                    "--candidate",
                    str(candidate_path),
                    "--output",
                    str(output),
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(1, completed.returncode, completed.stderr)
            self.assertFalse(json.loads(output.read_text(encoding="utf-8"))["passed"])
            repeated = subprocess.run(
                [
                    str(executable),
                    "--baseline",
                    str(baseline_path),
                    "--candidate",
                    str(candidate_path),
                    "--output",
                    str(output),
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertNotEqual(0, repeated.returncode)
            self.assertIn("refusing to overwrite", repeated.stderr)

    @staticmethod
    def _report(*, production: bool) -> dict[str, object]:
        return {
            "schema_version": 1,
            "report_type": "audio_detector_holdout_evaluation",
            "passed": True,
            "corpus_id": "frozen-corpus",
            "holdout_evidence_sha256": "e" * 64,
            "holdout_recording_ids": ["holdout-1", "holdout-2"],
            "policy_id": "production" if production else "candidate",
            "policy_lock_sha256": ("a" if production else "b") * 64,
            "detector": {
                "algorithm_id": (
                    "android-impact-detector-v1" if production else "candidate-detector-v2"
                ),
                "implementation_target": (
                    "//tools/field_evidence:production_audio_detector_replay"
                    if production
                    else "//example:candidate_detector"
                ),
                "implementation_sha256": ("c" if production else "d") * 64,
                "config_by_device": {"pixel5a": {}, "pixel6": {}},
            },
            "evaluation_policy": {
                "continuous_candidate_matching": "one_to_one_maximum_cardinality_v1",
                "minimum_target_recall_ppm": 1_000_000,
                "maximum_negative_continuous_candidates": 0,
            },
            "leakage_controls": {
                "development_recording_ids": ["development"],
                "policy_frozen_before_all_captures": True,
                "phone_view_swapped": True,
                "complete_timeline_review": True,
                "assets_hash_verified": True,
                "all_qualifying_holdouts_scored": True,
                "single_frozen_policy": True,
            },
            "checks": {},
            "candidate_generation": {
                "target_count": 20,
                "target_recalled": 19,
                "quiet_target_count": 4,
                "quiet_target_recalled": 4,
                "non_target_candidate_count": 2,
                "labeled_negative_candidates_by_category": {
                    "practice_swing": 1,
                    "speech": 1,
                },
            },
            "lifecycle": {
                "target_count": 10,
                "captured_target_count": 9,
                "false_terminal_attempt_count": 1,
                "reviewed_ms": 100_000,
                "high_speed_duty_ppm": 200_000,
            },
        }


if __name__ == "__main__":
    unittest.main()
