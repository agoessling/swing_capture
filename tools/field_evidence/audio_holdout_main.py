"""Command-line audio holdout qualification gate."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import final

from tools.field_evidence import audio_holdout, manifest, output


@final
class _Arguments(argparse.Namespace):
    """Typed command-line values populated by argparse."""

    def __init__(self) -> None:
        """Initialize defaults that argparse replaces for required options."""
        super().__init__()
        self.manifest = Path()
        self.media_root = Path()
        self.policy_lock = Path()
        self.predictions = Path()
        self.output = Path()


def _arguments() -> _Arguments:
    parser = argparse.ArgumentParser(
        description="score a frozen audio detector on hash-verified field holdouts"
    )
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--media-root", type=Path, required=True)
    parser.add_argument("--policy-lock", type=Path, required=True)
    parser.add_argument("--predictions", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args(namespace=_Arguments())


def main() -> int:
    """Evaluate the frozen detector policy against its holdout evidence."""
    arguments = _arguments()
    output.require_absent(arguments.output)
    corpus = manifest.parse_manifest(arguments.manifest.read_text(encoding="utf-8"))
    manifest.validate_assets(corpus, arguments.media_root)
    report = audio_holdout.evaluate_audio_holdout(
        corpus,
        arguments.policy_lock.read_text(encoding="utf-8"),
        arguments.predictions.read_text(encoding="utf-8"),
        assets_verified=True,
        source_capture_times_utc=audio_holdout.source_capture_times(corpus, arguments.media_root),
    )
    output.publish_json_exclusive(arguments.output, report)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
