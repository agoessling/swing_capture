"""Command-line entry point for the host pose-inference prototype."""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import sys
from pathlib import Path
from typing import TYPE_CHECKING, cast

from python.runfiles import runfiles

from tools.pose_inference import contract, mediapipe_runtime

if TYPE_CHECKING:
    from collections.abc import Sequence


@dataclasses.dataclass(frozen=True)
class _Options:
    input_path: Path
    model_path: Path | None
    output_path: Path | None


def _parse_options(arguments: Sequence[str] | None) -> _Options:
    parser = argparse.ArgumentParser(
        description="sample an MP4 at 5 fps and emit MediaPipe pose landmarks as NDJSON"
    )
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument(
        "--model",
        type=Path,
        help="override the Bazel-pinned MediaPipe Pose Landmarker Lite model",
    )
    parser.add_argument("--output", type=Path)
    parsed = parser.parse_args(arguments)
    return _Options(
        input_path=cast("Path", parsed.input),
        model_path=cast("Path | None", parsed.model),
        output_path=cast("Path | None", parsed.output),
    )


def _default_model_path() -> Path:
    resolver = runfiles.Create()
    if resolver is None:
        message = "Bazel runfiles are unavailable"
        raise RuntimeError(message)
    resolved = resolver.Rlocation("pose_landmarker_lite_task/file/pose_landmarker_lite.task")
    if not resolved:
        message = "Bazel runfiles did not contain the pinned pose landmarker model"
        raise RuntimeError(message)
    return Path(resolved)


def main(
    arguments: Sequence[str] | None = None,
    *,
    runtime_modules: mediapipe_runtime.RuntimeModules | None = None,
) -> int:
    """Run the fixed-rate prototype using Bazel-provided native wheels."""
    options = _parse_options(arguments)
    model_path = options.model_path or _default_model_path()
    cv2_module, mediapipe_module = runtime_modules or mediapipe_runtime.load_runtime_modules()
    with contextlib.ExitStack() as stack:
        reader = stack.enter_context(
            mediapipe_runtime.OpenCvVideoReader(cv2_module, options.input_path)
        )
        landmarker = stack.enter_context(
            mediapipe_runtime.MediaPipePoseLandmarker(mediapipe_module, model_path)
        )
        if options.output_path is None:
            output = sys.stdout
        else:
            output = stack.enter_context(options.output_path.open("w", encoding="utf-8"))
        sample_count = contract.run_inference(reader, landmarker, output)
    print(f"emitted {sample_count} samples", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
