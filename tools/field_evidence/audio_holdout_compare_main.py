"""Compare a candidate detector to production on the exact same holdout."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import cast, final

from tools.field_evidence import audio_holdout_compare, output


@final
class _Arguments(argparse.Namespace):
    """Typed command-line values populated by argparse."""

    def __init__(self) -> None:
        """Initialize defaults that argparse replaces for required options."""
        super().__init__()
        self.baseline = Path()
        self.candidate = Path()
        self.output = Path()


def _arguments() -> _Arguments:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args(namespace=_Arguments())


def main() -> int:
    """Compare two holdout reports and persist the result."""
    arguments = _arguments()
    output.require_absent(arguments.output)
    baseline = cast("dict[str, object]", json.loads(arguments.baseline.read_text(encoding="utf-8")))
    candidate = cast(
        "dict[str, object]", json.loads(arguments.candidate.read_text(encoding="utf-8"))
    )
    report = audio_holdout_compare.compare_reports(baseline, candidate)
    output.publish_json_exclusive(arguments.output, report)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
