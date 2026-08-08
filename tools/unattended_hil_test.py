"""Tests for the unattended hardware-in-the-loop supervisor."""

from __future__ import annotations

import datetime
import json
import sys
import tempfile
import unittest
from pathlib import Path
from typing import cast

from tools import run_unattended_hil as unattended_hil


class UnattendedHilTest(unittest.TestCase):
    """Verify supervision behavior without requiring camera hardware."""

    def _read_json_object(self, path: Path) -> dict[str, object]:
        parsed = cast("object", json.loads(path.read_text(encoding="utf-8")))
        if not isinstance(parsed, dict):
            message = f"expected a JSON object in {path}"
            raise TypeError(message)
        return cast("dict[str, object]", parsed)

    def test_stages_select_expected_targets_and_durations(self) -> None:
        """Map each stage name to the expected Bazel target and duration."""
        self.assertEqual(15, unattended_hil.STAGES["smoke"].duration_seconds)
        self.assertEqual(300, unattended_hil.STAGES["qualify"].duration_seconds)
        self.assertEqual(1800, unattended_hil.STAGES["soak"].duration_seconds)
        for name, stage in unattended_hil.STAGES.items():
            self.assertEqual(f"//capture/daheng:dual_camera_{name}_hil_test", stage.test_target)

    def test_positive_integer_setting_rejects_invalid_values(self) -> None:
        """Accept positive settings and reject malformed or nonpositive values."""
        self.assertEqual(
            60,
            unattended_hil.positive_integer_setting({}, "WATCHDOG", 60),
        )
        self.assertEqual(
            17,
            unattended_hil.positive_integer_setting({"WATCHDOG": "17"}, "WATCHDOG", 60),
        )
        for value in ("", "0", "-1", "1.5", "seconds"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                unattended_hil.positive_integer_setting({"WATCHDOG": value}, "WATCHDOG", 60)

    def test_repository_root_prefers_bazel_workspace(self) -> None:
        """Prefer Bazel's explicit workspace environment variable."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.assertEqual(
                root.resolve(),
                unattended_hil.repository_root({"BUILD_WORKSPACE_DIRECTORY": str(root)}, Path("/")),
            )

    def test_status_is_typed_and_published_atomically(self) -> None:
        """Publish typed status snapshots to run-specific and latest paths."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            started = datetime.datetime(2026, 8, 1, 12, 0, tzinfo=datetime.UTC)
            heartbeat = datetime.datetime(2026, 8, 1, 12, 1, tzinfo=datetime.UTC)
            paths = unattended_hil.RunPaths.create(root, "smoke", started, 123)
            writer = unattended_hil.StatusWriter(
                paths,
                "smoke",
                unattended_hil.STAGES["smoke"],
                60,
                started,
            )

            writer.write("running", message="active", now=heartbeat)
            status = self._read_json_object(paths.status)
            self.assertIsNone(status["completed_at"])
            self.assertIsNone(status["command_exit_code"])
            self.assertFalse(status["timed_out"])
            self.assertEqual("2026-08-01T12:01:00Z", status["heartbeat_at"])
            self.assertEqual(
                status,
                self._read_json_object(paths.latest_directory / "status.json"),
            )

            writer.write("failed", exit_code=7, now=heartbeat)
            status = self._read_json_object(paths.status)
            self.assertEqual(7, status["command_exit_code"])
            self.assertEqual("2026-08-01T12:01:00Z", status["completed_at"])

    def test_camera_serials_prefer_report_and_fall_back_to_log(self) -> None:
        """Prefer structured serials and recover from malformed reports via logs."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            report = root / "report.json"
            log = root / "run.log"
            log.write_text(
                "USB: bus=1 serial=LOG2 other=x\nUSB: bus=2 serial=LOG1 other=y\n",
                encoding="utf-8",
            )
            self.assertEqual(["LOG1", "LOG2"], unattended_hil.camera_serials(report, log))

            report.write_text(
                json.dumps(
                    {
                        "cameras": [
                            {"serial": "REPORT2"},
                            {"serial": "REPORT1"},
                            {"missing": "serial"},
                        ]
                    }
                ),
                encoding="utf-8",
            )
            self.assertEqual(
                ["REPORT1", "REPORT2"],
                unattended_hil.camera_serials(report, log),
            )

            report.write_text("[]", encoding="utf-8")
            self.assertEqual(["LOG1", "LOG2"], unattended_hil.camera_serials(report, log))

    def test_publish_test_artifacts_copies_report_and_frames(self) -> None:
        """Copy reports and diagnostic frames into durable artifact locations."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            started = datetime.datetime(2026, 8, 1, tzinfo=datetime.UTC)
            stage = unattended_hil.STAGES["smoke"]
            paths = unattended_hil.RunPaths.create(root, "smoke", started, 123)
            outputs = (
                root / "bazel-testlogs" / "capture" / "daheng" / stage.test_name / "test.outputs"
            )
            outputs.mkdir(parents=True)
            (outputs / "report.json").write_text('{"passed": true}\n', encoding="utf-8")
            (outputs / "frame-CAMERA.png").write_bytes(b"png")

            unattended_hil.publish_test_artifacts(root, stage, paths)

            self.assertTrue(unattended_hil.report_passed(paths.report))
            self.assertEqual(b"png", (paths.run_directory / "frame-CAMERA.png").read_bytes())
            self.assertEqual(b"png", (paths.latest_directory / "frame-CAMERA.png").read_bytes())

    def test_command_supervisor_streams_output_and_enforces_watchdog(self) -> None:
        """Stream child output and terminate commands that exceed the watchdog."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            started = datetime.datetime(2026, 8, 1, tzinfo=datetime.UTC)
            paths = unattended_hil.RunPaths.create(root, "smoke", started, 123)
            statuses = unattended_hil.StatusWriter(
                paths,
                "smoke",
                unattended_hil.STAGES["smoke"],
                60,
                started,
            )
            supervisor = unattended_hil.CommandSupervisor(
                root, paths.log, statuses, heartbeat_seconds=10
            )

            completed = supervisor.run(
                [sys.executable, "-c", "print('supervised output')"],
                state="running",
                watchdog_seconds=5,
            )
            self.assertEqual(0, completed.exit_code)
            self.assertIn("supervised output", paths.log.read_text(encoding="utf-8"))

            timed_out = supervisor.run(
                [sys.executable, "-c", "import time; time.sleep(30)"],
                state="running",
                watchdog_seconds=0.1,
            )
            self.assertEqual(124, timed_out.exit_code)
            self.assertTrue(timed_out.timed_out)


if __name__ == "__main__":
    unittest.main()
