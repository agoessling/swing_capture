"""Executable-boundary smoke for the audio holdout qualification gate."""

from __future__ import annotations

import hashlib
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import cast

from python.runfiles import runfiles

from tools.field_evidence.audio_holdout_test import (
    complete_corpus_value,
    policy_lock_value,
    prediction_value,
)


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


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _write_fixture(root: Path, *, failing_acceptance: bool) -> tuple[Path, Path, Path]:
    corpus = complete_corpus_value()
    lock = policy_lock_value()
    if failing_acceptance:
        evaluation = cast("dict[str, object]", lock["evaluation"])
        evaluation["maximum_high_speed_duty_ppm"] = 0
    for raw_session in cast("list[object]", corpus["sessions"]):
        session = cast("dict[str, object]", raw_session)
        for role in ("down_the_line", "face_on"):
            stream = cast(
                "dict[str, object]",
                cast("dict[str, object]", session["streams"])[role],
            )
            video_asset = cast("dict[str, object]", stream["video"])
            audio_asset = cast("dict[str, object]", stream["audio"])
            source_asset = cast("dict[str, object]", stream["source_manifest"])
            video_path = root / cast("str", video_asset["path"])
            audio_path = root / cast("str", audio_asset["path"])
            source_path = root / cast("str", source_asset["path"])
            video_path.parent.mkdir(parents=True, exist_ok=True)
            video_path.write_bytes(f"video-{role}".encode())
            audio_path.write_bytes(f"audio-{role}".encode())
            source_path.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "session_kind": "field_recording",
                        "shared_recording_id": session["recording_id"],
                        "node_id": stream["node_id"],
                        "role": role,
                        "created_at_utc": "2026-08-23T00:00:00Z",
                        "duration_us": "27000000",
                        "video": {"bytes": str(video_path.stat().st_size)},
                        "audio": {"bytes": str(audio_path.stat().st_size)},
                    }
                ),
                encoding="utf-8",
            )
            video_asset["sha256"] = _sha256(video_path)
            audio_asset["sha256"] = _sha256(audio_path)
            source_asset["sha256"] = _sha256(source_path)
    manifest_path = root / "evidence_manifest.json"
    lock_path = root / "policy_lock.json"
    predictions_path = root / "predictions.json"
    manifest_path.write_text(json.dumps(corpus), encoding="utf-8")
    lock_path.write_text(json.dumps(lock), encoding="utf-8")
    predictions_path.write_text(json.dumps(prediction_value(corpus, lock)), encoding="utf-8")
    return manifest_path, lock_path, predictions_path


class AudioHoldoutExecutableTest(unittest.TestCase):
    """Exercise the CLI result, rejection, and non-overwrite boundaries."""

    def test_success_failure_and_non_overwrite_boundaries(self) -> None:
        """Require stable success and failure exit behavior with durable reports."""
        executable = _runfile("tools/field_evidence/audio_holdout_gate")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest_path, lock_path, predictions_path = _write_fixture(
                root, failing_acceptance=False
            )
            passed_output = root / "passed.json"
            passed = self._run(
                executable, root, manifest_path, lock_path, predictions_path, passed_output
            )
            self.assertEqual(0, passed.returncode, passed.stderr)
            passed_report = cast(
                "dict[str, object]", json.loads(passed_output.read_text(encoding="utf-8"))
            )
            self.assertTrue(passed_report["passed"])

            repeated = self._run(
                executable, root, manifest_path, lock_path, predictions_path, passed_output
            )
            self.assertNotEqual(0, repeated.returncode)
            self.assertIn("refusing to overwrite existing evidence", repeated.stderr)

            failing_root = root / "failing"
            failing_root.mkdir()
            failing_manifest, failing_lock, failing_predictions = _write_fixture(
                failing_root, failing_acceptance=True
            )
            failed_output = failing_root / "failed.json"
            failed = self._run(
                executable,
                failing_root,
                failing_manifest,
                failing_lock,
                failing_predictions,
                failed_output,
            )
            self.assertEqual(1, failed.returncode, failed.stderr)
            failed_report = cast(
                "dict[str, object]", json.loads(failed_output.read_text(encoding="utf-8"))
            )
            failed_checks = cast("dict[str, object]", failed_report["checks"])
            self.assertFalse(failed_report["passed"])
            self.assertFalse(failed_checks["high_speed_duty"])

    @staticmethod
    def _run(  # noqa: PLR0913 - explicit paths make each CLI input auditable.
        executable: Path,
        root: Path,
        manifest_path: Path,
        lock_path: Path,
        predictions_path: Path,
        output: Path,
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                str(executable),
                "--manifest",
                str(manifest_path),
                "--media-root",
                str(root),
                "--policy-lock",
                str(lock_path),
                "--predictions",
                str(predictions_path),
                "--output",
                str(output),
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )


if __name__ == "__main__":
    unittest.main()
