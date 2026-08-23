"""Command-line entry point for bounded Android field-session preflight."""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path
from typing import TYPE_CHECKING, final

from tools.field_preflight.admission import (
    AdmissionError,
    ExpectedRole,
    execute,
    parse_expected_role,
    parse_node,
)
from tools.field_preflight.admission import Node as FieldNode

if TYPE_CHECKING:
    from collections.abc import Sequence


@final
class _Arguments(argparse.Namespace):
    """Typed values populated by the command-line parser."""

    def __init__(self) -> None:
        """Initialize parser defaults for static analysis."""
        super().__init__()
        self.adb: Path = Path()
        self.doctor: Path = Path()
        self.expected_apk: Path = Path()
        self.node: list[FieldNode] = []
        self.expected_role: list[ExpectedRole] = []
        self.evidence_dir: Path = Path()
        self.launch_after_unlock: bool = False
        self.sleep_screen_after_launch: bool = False
        self.require_monitoring: bool = False
        self.maximum_attempts: int = 3
        self.retry_seconds: float = 2.0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Discover and reconnect two wireless-ADB endpoints, verify authorization/unlock, "
            "optionally "
            "launch the app, and retry the strict current-APK pair doctor while peer recovery "
            "converges. This tool never reboots, unlocks, pairs, configures, or arms a phone."
        )
    )
    parser.add_argument("adb", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("doctor", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("expected_apk", type=Path, help=argparse.SUPPRESS)
    parser.add_argument(
        "--node",
        action="append",
        required=True,
        type=parse_node,
        metavar="HOST[:ADB_PORT]=http://HOST:APP_PORT",
    )
    parser.add_argument(
        "--expected-role",
        action="append",
        required=True,
        type=parse_expected_role,
        metavar="HOST=face_on|down_the_line",
        help="expected configured camera role for each stable phone host; specify exactly twice",
    )
    parser.add_argument("--evidence-dir", required=True, type=Path)
    parser.add_argument(
        "--launch-after-unlock",
        action="store_true",
        help="perform an explicit ADB foreground activity launch after verifying user unlock",
    )
    parser.add_argument(
        "--sleep-screen-after-launch",
        action="store_true",
        help="send KEYCODE_SLEEP after launch and require a non-interactive/dozing power state",
    )
    parser.add_argument(
        "--require-monitoring",
        action="store_true",
        help="require both phones armed with live pose/audio and recovered peer timing",
    )
    parser.add_argument("--maximum-attempts", type=int, default=3)
    parser.add_argument("--retry-seconds", type=float, default=2.0)
    return parser


def _workspace_path(path: Path) -> Path:
    """Keep documented relative evidence paths in the invoking Bazel workspace."""
    if path.is_absolute():
        return path
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    return Path(workspace, path) if workspace else path


def main(arguments: Sequence[str] | None = None) -> int:
    """Run bounded pair admission and print its operator-facing summary."""
    options = _parser().parse_args(arguments, namespace=_Arguments())
    try:
        report = execute(
            options.adb,
            options.doctor,
            options.node,
            _workspace_path(options.evidence_dir),
            expected_roles=options.expected_role,
            launch_after_unlock=options.launch_after_unlock,
            sleep_screen_after_launch=options.sleep_screen_after_launch,
            require_monitoring=options.require_monitoring,
            maximum_attempts=options.maximum_attempts,
            retry_seconds=options.retry_seconds,
            expected_apk=options.expected_apk,
        )
    except (AdmissionError, OSError, ValueError) as failure:
        print(f"android field preflight: {failure}", file=sys.stderr)
        return 2
    for node in report["nodes"]:
        marker = "PASS" if node["passed"] else "FAIL"
        print(f"[{marker}] {node['serial']}: {', '.join(node['issues']) or 'transport ready'}")
    for association in report["role_associations"]:
        marker = "PASS" if association["passed"] else "FAIL"
        host = association["host"]
        expected_role = association["expected_role"]
        observed_role = association["observed_role"] or "none"
        print(f"[{marker}] {host}: expected role {expected_role}, observed {observed_role}")
    marker = "PASS" if report["passed"] else "FAIL"
    print(f"[{marker}] strict pair admission: {len(report['doctor_attempts'])} attempt(s)")
    if not report["passed"] and report["doctor_attempts"]:
        for failure in report["doctor_attempts"][-1]["failures"]:
            print(f"[FAIL] {failure['name']}: {failure['message']}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
