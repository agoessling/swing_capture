"""Command-line entry point for continuous phone field-evidence collection."""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import TYPE_CHECKING, final

from tools.field_feedback.collector import (
    CollectionError,
    FieldFeedbackCollector,
    NodeSpec,
)

if TYPE_CHECKING:
    from collections.abc import Sequence


@final
class ParsedOptions(argparse.Namespace):
    """Typed view of arguments populated by ``build_parser``."""

    node: list[str]
    token: list[str]
    output: Path
    poll_interval_seconds: float
    request_timeout_seconds: float
    once: bool
    extract_previews: bool

    def __init__(self) -> None:
        """Initialize values that argparse will replace from its declared actions."""
        super().__init__()
        self.node = []
        self.token = []
        self.output = Path()
        self.poll_interval_seconds = 0.0
        self.request_timeout_seconds = 0.0
        self.once = False
        self.extract_previews = False


def build_parser() -> argparse.ArgumentParser:
    """Build the deliberately explicit one/two-phone command-line contract."""
    parser = argparse.ArgumentParser(
        description="Collect validated diagnostic ZIPs from one or two Android capture phones.",
    )
    parser.add_argument(
        "--node",
        action="append",
        required=True,
        metavar="BASE_URL",
        help="bare phone origin; repeat once per phone",
    )
    parser.add_argument(
        "--token",
        action="append",
        required=True,
        metavar="TOKEN",
        help="matching 32-character control token; repeat in --node order",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("artifacts/field_feedback"),
        help="collector-owned artifact directory",
    )
    parser.add_argument(
        "--poll-interval-seconds",
        type=float,
        default=5.0,
        help="continuous discovery interval",
    )
    parser.add_argument(
        "--request-timeout-seconds",
        type=float,
        default=10.0,
        help="per-request timeout",
    )
    parser.add_argument(
        "--once",
        action="store_true",
        help="run one discovery pass and exit",
    )
    parser.add_argument(
        "--extract-previews",
        action="store_true",
        help="materialize preview_frames.mjpeg, pose_trace.ndjson, and a byte-span frame index",
    )
    return parser


def specifications(nodes: Sequence[str], tokens: Sequence[str]) -> tuple[NodeSpec, ...]:
    """Pair repeated URL/token flags without accepting implicit credentials."""
    if len(nodes) not in (1, 2) or len(nodes) != len(tokens):
        message = "provide one or two --node values and exactly one matching --token per node"
        raise ValueError(message)
    return tuple(NodeSpec(node, token) for node, token in zip(nodes, tokens, strict=True))


def main(arguments: Sequence[str] | None = None) -> int:
    """Run once for automation or poll until interrupted for field collection."""
    parser = build_parser()
    options = parser.parse_args(arguments, namespace=ParsedOptions())
    if options.poll_interval_seconds <= 0:
        parser.error("--poll-interval-seconds must be positive")
    try:
        nodes = specifications(options.node, options.token)
        collector = FieldFeedbackCollector(
            nodes,
            options.output,
            timeout_seconds=options.request_timeout_seconds,
            extract_previews=options.extract_previews,
        )
    except (ValueError, CollectionError) as error:
        parser.error(str(error))

    exit_code = 0
    try:
        while True:
            result = collector.collect_once()
            print(json.dumps(result.as_json(), sort_keys=True), flush=True)
            if result.errors:
                exit_code = 1
            if options.once:
                return exit_code
            time.sleep(options.poll_interval_seconds)
    except KeyboardInterrupt:
        return exit_code
    except CollectionError as error:
        print(f"field feedback collector: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
