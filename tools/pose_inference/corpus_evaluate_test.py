"""Hermetic tests for pose corpus batch planning and aggregation."""

from __future__ import annotations

import contextlib
import dataclasses
import io
import json
import unittest
from pathlib import Path
from typing import cast

from tools.pose_inference import corpus, corpus_evaluate


class _FakeRunfilesResolver:
    """Deterministic runfiles resolver for model-packaging tests."""

    def Rlocation(self, logical_path: str) -> str:  # noqa: N802
        return f"/runfiles/{logical_path}"


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
            self.fail("checked-in legacy ROI provenance unexpectedly disappeared")
        self.assertEqual("0.35,0.55,0.68,0.98", region.encoded())

    def test_evaluation_is_full_frame_with_or_without_legacy_roi(self) -> None:
        """Never let per-clip setup rectangles alter future qualification."""
        manifest = self._manifest()
        first = manifest.clips[0]
        without_roi = dataclasses.replace(first, clip_id="IN01NOROI", hitting_region=None)
        jobs = corpus_evaluate.build_jobs(
            dataclasses.replace(manifest, clips=(first, without_roi)),
            Path("/media"),
            Path("/output"),
        )

        for job in jobs:
            with self.subTest(clip_id=job.clip.clip_id):
                self.assertEqual(
                    [
                        "/bin/pose_landmarks_to_observations",
                        f"--landmarks={job.output_directory / 'landmarks.csv'}",
                        f"--observations={job.output_directory / 'observations.csv'}",
                        "--hitting-region=0,0,1,1",
                        f"--projection={job.clip.view}",
                    ],
                    corpus_evaluate.observation_adapter_arguments(
                        job, Path("/bin/pose_landmarks_to_observations")
                    ),
                )

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
        self.assertEqual("lite", report["model_variant"])
        self.assertEqual("full_frame", report["spatial_evaluation_policy"])
        self.assertEqual("required_command_line", report["startup_budget_source"])
        excluded = cast("list[dict[str, object]]", report["excluded"])
        self.assertIn("C02", {value["id"] for value in excluded})

    def test_model_variants_select_only_their_pinned_assets(self) -> None:
        """Keep the comparison closed and pass the selected model explicitly."""
        assets = corpus_evaluate.ModelAssets(
            lite=Path("/models/pose_landmarker_lite.task"),
            full=Path("/models/pose_landmarker_full.task"),
            heavy=Path("/models/pose_landmarker_heavy.task"),
        )
        job = corpus_evaluate.build_jobs(self._manifest(), Path("/media"), Path("/output"))[0]
        expected_models = {
            corpus_evaluate.ModelVariant.LITE: assets.lite,
            corpus_evaluate.ModelVariant.FULL: assets.full,
            corpus_evaluate.ModelVariant.HEAVY: assets.heavy,
        }
        for variant, expected_model in expected_models.items():
            with self.subTest(variant=variant.value):
                self.assertEqual(expected_model, assets.select(variant))
                self.assertEqual(
                    [
                        "/bin/pose_landmarker",
                        f"--input={job.media_path}",
                        f"--model={expected_model}",
                        f"--output={job.output_directory / 'landmarks.ndjson'}",
                    ],
                    corpus_evaluate.landmarker_arguments(
                        job, Path("/bin/pose_landmarker"), assets.select(variant)
                    ),
                )

        with self.assertRaises(ValueError):
            corpus_evaluate.ModelVariant("unsupported")

    def test_resolves_every_official_model_from_runfiles(self) -> None:
        """Do not make Full or Heavy depend on a developer-checkout path."""
        executables = corpus_evaluate.Executables.from_resolver(_FakeRunfilesResolver())

        self.assertEqual(
            Path("/runfiles/pose_landmarker_lite_task/file/pose_landmarker_lite.task"),
            executables.models.lite,
        )
        self.assertEqual(
            Path("/runfiles/pose_landmarker_full_task/file/pose_landmarker_full.task"),
            executables.models.full,
        )
        self.assertEqual(
            Path("/runfiles/pose_landmarker_heavy_task/file/pose_landmarker_heavy.task"),
            executables.models.heavy,
        )

    def test_aggregate_records_selected_model_variant(self) -> None:
        """Make reports self-describing when comparing otherwise identical runs."""
        report = corpus_evaluate.aggregate(
            self._manifest(), [], 800, corpus_evaluate.ModelVariant.HEAVY
        )
        self.assertEqual("heavy", report["model_variant"])

    def test_cli_rejects_model_paths_and_unknown_variants(self) -> None:
        """Expose a closed comparison switch instead of accepting ad-hoc assets."""
        with (
            contextlib.redirect_stderr(io.StringIO()),
            self.assertRaises(SystemExit) as raised,
        ):
            corpus_evaluate.main(
                [
                    "--manifest=/manifest.json",
                    "--media-root=/media",
                    "--output-root=/output",
                    "--model-variant=/tmp/custom.task",
                ]
            )
        self.assertEqual(2, raised.exception.code)

    def test_cli_requires_explicit_startup_budget(self) -> None:
        """Do not silently score a corpus against an obsolete device budget."""
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit) as raised:
            corpus_evaluate.main(
                [
                    "--manifest=/manifest.json",
                    "--media-root=/media",
                    "--output-root=/output",
                ]
            )
        self.assertEqual(2, raised.exception.code)
        self.assertIn("--startup-budget-ms", stderr.getvalue())

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
