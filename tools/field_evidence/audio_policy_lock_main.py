"""Safely freeze one pre-capture audio-detector policy lock."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import cast, final

from tools.field_evidence import audio_policy_lock, output


@final
class _Arguments(argparse.Namespace):
    """Typed command-line values populated by argparse."""

    def __init__(self) -> None:
        """Initialize defaults that argparse replaces for required options."""
        super().__init__()
        self.policy_id = ""
        self.frozen_at_utc = ""
        self.development_recording_id: list[str] = []
        self.development_down_the_line_device = ""
        self.development_face_on_device = ""
        self.detector = Path()
        self.output = Path()


def _arguments() -> _Arguments:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--policy-id", required=True)
    parser.add_argument("--frozen-at-utc", required=True)
    parser.add_argument("--development-recording-id", action="append", required=True)
    parser.add_argument("--development-down-the-line-device", required=True)
    parser.add_argument("--development-face-on-device", required=True)
    parser.add_argument("--detector", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args(namespace=_Arguments())


def main() -> int:
    """Create and exclusively publish a validated detector policy lock."""
    arguments = _arguments()
    output.require_absent(arguments.output)
    detector = cast("object", json.loads(arguments.detector.read_text(encoding="utf-8")))
    value = audio_policy_lock.create_policy_lock(
        policy_id=arguments.policy_id,
        frozen_at_utc=arguments.frozen_at_utc,
        development_recording_ids=arguments.development_recording_id,
        development_device_assignment={
            "down_the_line": arguments.development_down_the_line_device,
            "face_on": arguments.development_face_on_device,
        },
        detector=detector,
    )
    output.publish_json_exclusive(arguments.output, value)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
