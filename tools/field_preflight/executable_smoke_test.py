"""End-to-end runfiles smoke for the operator-facing field-preflight binary."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import cast

from python.runfiles import runfiles


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


class ExecutableSmokeTest(unittest.TestCase):
    """Invoke the packaged binary through fake process boundaries."""

    def test_operator_executable_runs_complete_strict_ceremony(self) -> None:
        """Require the packaged command to retain a passing strict report."""
        executable = _runfile("tools/field_preflight/android_field_preflight_test_fixture")
        fake_adb = _runfile("tools/field_preflight/fake_adb")
        fake_doctor = _runfile("tools/field_preflight/fake_doctor")
        with tempfile.TemporaryDirectory() as temporary:
            expected_apk = Path(temporary) / "expected.apk"
            expected_apk.write_bytes(b"fake exact APK\n")
            evidence = Path(temporary) / "artifacts" / "field"
            result = subprocess.run(
                [
                    str(executable),
                    str(fake_adb),
                    str(fake_doctor),
                    str(expected_apk),
                    "--node",
                    "10.168.168.111:37123=http://10.168.168.111:8088",
                    "--node",
                    "10.168.168.241:42491=http://10.168.168.241:8088",
                    "--expected-role",
                    "10.168.168.111=face_on",
                    "--expected-role",
                    "10.168.168.241=down_the_line",
                    "--evidence-dir",
                    "artifacts/field",
                    "--launch-after-unlock",
                    "--sleep-screen-after-launch",
                    "--require-monitoring",
                    "--maximum-attempts",
                    "1",
                    "--retry-seconds",
                    "0",
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=15,
                env={**os.environ, "BUILD_WORKSPACE_DIRECTORY": temporary},
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((evidence / "report.json").is_file(), result.stdout)
            report = cast(
                "dict[str, object]",
                json.loads((evidence / "report.json").read_text(encoding="utf-8")),
            )

        self.assertIn("[PASS] strict pair admission: 1 attempt(s)", result.stdout)
        self.assertTrue(report["passed"])
        self.assertTrue(report["require_monitoring"])
        self.assertEqual(report["launch_mode"], "adb_foreground_activity")
        self.assertEqual(report["screen_policy"], "sleep_after_launch")
        role_associations = cast("list[dict[str, object]]", report["role_associations"])
        self.assertTrue(all(association["passed"] is True for association in role_associations))
        nodes = cast("list[dict[str, object]]", report["nodes"])
        self.assertEqual(len(nodes), 2)
        self.assertTrue(all(node["passed"] is True for node in nodes))
        attempts = cast("list[dict[str, object]]", report["doctor_attempts"])
        self.assertEqual(attempts[0]["passed"], True)
        self.assertEqual(attempts[0]["status_diagnostics_passed"], True)

    def test_operator_executable_forwards_stopped_clean_gate(self) -> None:
        """Require the packaged command to forward and retain terminal admission semantics."""
        executable = _runfile("tools/field_preflight/android_field_preflight_test_fixture")
        fake_adb = _runfile("tools/field_preflight/fake_adb")
        fake_doctor = _runfile("tools/field_preflight/fake_doctor")
        with tempfile.TemporaryDirectory() as temporary:
            expected_apk = Path(temporary) / "expected.apk"
            expected_apk.write_bytes(b"fake exact APK\n")
            evidence = Path(temporary) / "artifacts" / "field-stopped"
            result = subprocess.run(
                [
                    str(executable),
                    str(fake_adb),
                    str(fake_doctor),
                    str(expected_apk),
                    "--node",
                    "10.168.168.111:37123=http://10.168.168.111:8088",
                    "--node",
                    "10.168.168.241:42491=http://10.168.168.241:8088",
                    "--expected-role",
                    "10.168.168.111=face_on",
                    "--expected-role",
                    "10.168.168.241=down_the_line",
                    "--evidence-dir",
                    "artifacts/field-stopped",
                    "--require-stopped-clean",
                    "--maximum-attempts",
                    "1",
                    "--retry-seconds",
                    "0",
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=15,
                env={**os.environ, "BUILD_WORKSPACE_DIRECTORY": temporary},
            )
            report = cast(
                "dict[str, object]",
                json.loads((evidence / "report.json").read_text(encoding="utf-8")),
            )
            child = cast(
                "dict[str, object]",
                json.loads((evidence / "doctor_attempt_1.json").read_text(encoding="utf-8")),
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(report["passed"])
        self.assertFalse(report["require_monitoring"])
        self.assertTrue(report["require_stopped_clean"])
        attempts = cast("list[dict[str, object]]", report["doctor_attempts"])
        self.assertTrue(attempts[0]["status_diagnostics_passed"])
        nodes = cast("list[dict[str, object]]", child["nodes"])
        status = cast("dict[str, object]", nodes[0]["capture_status"])
        field_status = cast("dict[str, object]", nodes[0]["field_recording_status"])
        self.assertEqual(status["ring_bytes"], 12_345_678)
        self.assertEqual(field_status["state"], "idle")
        self.assertEqual(field_status["max_duration_seconds"], 600)

    def test_operator_executable_surfaces_actionable_strict_doctor_failure(self) -> None:
        """Require a failing strict doctor check to reach the operator output."""
        executable = _runfile("tools/field_preflight/android_field_preflight_test_fixture")
        fake_adb = _runfile("tools/field_preflight/fake_adb")
        fake_doctor = _runfile("tools/field_preflight/fake_doctor")
        with tempfile.TemporaryDirectory() as temporary:
            expected_apk = Path(temporary) / "expected.apk"
            expected_apk.write_bytes(b"fake exact APK\n")
            result = subprocess.run(
                [
                    str(executable),
                    str(fake_adb),
                    str(fake_doctor),
                    str(expected_apk),
                    "--node",
                    "10.168.168.111:37123=http://10.168.168.111:8088",
                    "--node",
                    "10.168.168.241:42491=http://10.168.168.241:8088",
                    "--expected-role",
                    "10.168.168.111=face_on",
                    "--expected-role",
                    "10.168.168.241=down_the_line",
                    "--evidence-dir",
                    "artifacts/field-failure",
                    "--maximum-attempts",
                    "1",
                    "--retry-seconds",
                    "0",
                    "--require-monitoring",
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=15,
                env={
                    **os.environ,
                    "BUILD_WORKSPACE_DIRECTORY": temporary,
                    "SWING_CAPTURE_FAKE_DOCTOR_FAIL": "1",
                },
            )

        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("[FAIL] strict pair admission: 1 attempt(s)", result.stdout)
        self.assertIn(
            "[FAIL] node.1.exact_apk: installed APK differs from the requested Bazel artifact",
            result.stdout,
        )


if __name__ == "__main__":
    unittest.main()
