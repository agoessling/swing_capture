"""CLI for the bounded paired hitting-session evidence sidecar."""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path
from typing import final

from tools.session_sidecar.collector import (
    DEFAULT_INTERVAL_SECONDS,
    DEFAULT_MAXIMUM_ARTIFACT_BYTES,
    DEFAULT_MAXIMUM_LOGCAT_BYTES,
    Configuration,
    Node,
    SidecarError,
    collect,
    parse_node,
)

PAIR_NODE_COUNT = 2


@final
class _Arguments(argparse.Namespace):
    def __init__(self) -> None:
        super().__init__()
        self.adb = Path()
        self.node: list[Node] = []
        self.output_dir = Path()
        self.duration_seconds = 0.0
        self.interval_seconds = DEFAULT_INTERVAL_SECONDS
        self.maximum_logcat_bytes = DEFAULT_MAXIMUM_LOGCAT_BYTES
        self.maximum_artifact_bytes = DEFAULT_MAXIMUM_ARTIFACT_BYTES


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Retain bounded read-only status, logcat, power, battery, thermal, and Wi-Fi evidence "
            "for one paired hitting session. The tool never arms, stops, configures, deletes, or "
            "exports media."
        )
    )
    parser.add_argument("adb", type=Path, help=argparse.SUPPRESS)
    parser.add_argument(
        "--node",
        action="append",
        required=True,
        type=parse_node,
        metavar="SERIAL=http://HOST:PORT",
        help="ADB serial and credential-free direct-LAN app origin; specify exactly twice",
    )
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--duration-seconds", required=True, type=float)
    parser.add_argument("--interval-seconds", type=float, default=DEFAULT_INTERVAL_SECONDS)
    parser.add_argument(
        "--maximum-logcat-bytes",
        type=int,
        default=DEFAULT_MAXIMUM_LOGCAT_BYTES,
        help="retained tail bound per phone",
    )
    parser.add_argument(
        "--maximum-artifact-bytes",
        type=int,
        default=DEFAULT_MAXIMUM_ARTIFACT_BYTES,
        help="hard bound for the complete sidecar directory",
    )
    return parser


def _workspace_path(path: Path) -> Path:
    if path.is_absolute():
        return path
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    return Path(workspace, path) if workspace else path


def _configuration(options: _Arguments) -> Configuration:
    if len(options.node) != PAIR_NODE_COUNT:
        message = "--node must be specified exactly twice"
        raise ValueError(message)
    return Configuration(
        adb=options.adb,
        nodes=(options.node[0], options.node[1]),
        output_directory=_workspace_path(options.output_dir),
        duration_seconds=options.duration_seconds,
        interval_seconds=options.interval_seconds,
        maximum_logcat_bytes=options.maximum_logcat_bytes,
        maximum_artifact_bytes=options.maximum_artifact_bytes,
    )


def main(arguments: list[str] | None = None) -> int:
    """Collect one bounded, read-only paired-session evidence directory."""
    options = _parser().parse_args(arguments, namespace=_Arguments())
    try:
        report = collect(_configuration(options))
    except (OSError, SidecarError, ValueError) as failure:
        print(f"session sidecar: {failure}", file=sys.stderr)
        return 2
    print(f"session sidecar: retained {report['snapshot_count']} snapshots in {options.output_dir}")
    if report["interrupted"]:
        return 130
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
