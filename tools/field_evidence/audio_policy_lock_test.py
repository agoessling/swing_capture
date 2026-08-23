"""Tests for immutable audio policy lock creation."""

from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from python.runfiles import runfiles

from tools.field_evidence import audio_holdout, audio_policy_lock


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


def _detector() -> dict[str, object]:
    return {
        "algorithm_id": "test-detector-v1",
        "implementation_target": "//test:replay",
        "implementation_sha256": "a" * 64,
        "config_by_device": {"pixel5a": {"threshold": 1}, "pixel6": {"threshold": 2}},
    }


class AudioPolicyLockTest(unittest.TestCase):
    """Verify immutable detector and evaluation policy lock creation."""

    def test_builds_parseable_lock_with_fixed_evaluation_policy(self) -> None:
        """Create a lock accepted by the downstream holdout parser."""
        value = self._create(_detector())
        parsed = audio_holdout.parse_policy_lock(json.dumps(value))
        self.assertEqual(("field-development-001",), parsed.development_recording_ids)
        self.assertEqual(2_450, parsed.evaluation.audio_ready_delay_ms)
        self.assertEqual(300_000, parsed.evaluation.maximum_high_speed_duty_ppm)

    def test_rejects_detector_device_mismatch(self) -> None:
        """Reject detector configs that omit one development device."""
        detector = _detector()
        detector["config_by_device"] = {"pixel5a": {}}
        with self.assertRaisesRegex(ValueError, "exactly match"):
            self._create(detector)

    def test_executable_refuses_to_overwrite_lock(self) -> None:
        """Keep a frozen policy byte-stable after a repeated CLI invocation."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            detector = root / "detector.json"
            output = root / "policy.json"
            detector.write_text(json.dumps(_detector()), encoding="utf-8")
            command = [
                str(_runfile("tools/field_evidence/audio_policy_lock")),
                "--policy-id",
                "candidate-v1",
                "--frozen-at-utc",
                "2026-08-23T12:00:00Z",
                "--development-recording-id",
                "field-development-001",
                "--development-down-the-line-device",
                "pixel5a",
                "--development-face-on-device",
                "pixel6",
                "--detector",
                str(detector),
                "--output",
                str(output),
            ]
            completed = subprocess.run(
                command, check=False, capture_output=True, text=True, timeout=10
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            first_bytes = output.read_bytes()
            repeated = subprocess.run(
                command, check=False, capture_output=True, text=True, timeout=10
            )
            self.assertNotEqual(0, repeated.returncode)
            self.assertIn("refusing to overwrite", repeated.stderr)
            self.assertEqual(first_bytes, output.read_bytes())

    @staticmethod
    def _create(detector: object) -> dict[str, object]:
        return audio_policy_lock.create_policy_lock(
            policy_id="candidate-v1",
            frozen_at_utc="2026-08-23T12:00:00Z",
            development_recording_ids=["field-development-001"],
            development_device_assignment={
                "down_the_line": "pixel5a",
                "face_on": "pixel6",
            },
            detector=detector,
        )


if __name__ == "__main__":
    unittest.main()
