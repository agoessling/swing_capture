"""Run the Bazel-owned pose pipeline over every supported corpus clip."""

from __future__ import annotations

import argparse
import dataclasses
import json
import subprocess
from pathlib import Path
from typing import TYPE_CHECKING, cast

from python.runfiles import runfiles

from tools.pose_inference import corpus

if TYPE_CHECKING:
    from collections.abc import Sequence


NANOS_PER_MILLISECOND = 1_000_000


@dataclasses.dataclass(frozen=True)
class Executables:
    """Runfile-resolved Bazel stage binaries."""

    landmarker: Path
    landmark_csv: Path
    observation_adapter: Path
    replay: Path
    annotator: Path

    @classmethod
    def from_runfiles(cls) -> Executables:
        """Resolve every stage without relying on a developer checkout layout."""
        resolver = runfiles.Create()
        if resolver is None:
            message = "Bazel runfiles are unavailable"
            raise RuntimeError(message)

        def resolve(logical_path: str) -> Path:
            resolved = resolver.Rlocation(f"_main/{logical_path}")
            if not resolved:
                message = f"missing Bazel runfile: {logical_path}"
                raise RuntimeError(message)
            return Path(resolved)

        return cls(
            landmarker=resolve("tools/pose_inference/pose_landmarker"),
            landmark_csv=resolve("tools/pose_inference/landmark_csv"),
            observation_adapter=resolve("android/core/pose/pose_landmarks_to_observations"),
            replay=resolve("android/core/pose/pose_trigger_replay"),
            annotator=resolve("tools/pose_inference/annotate_pose_trigger"),
        )


@dataclasses.dataclass(frozen=True)
class EvaluationJob:
    """One positive clip and its isolated output directory."""

    clip: corpus.Clip
    media_path: Path
    output_directory: Path


def build_jobs(
    manifest: corpus.Corpus, media_root: Path, output_root: Path
) -> tuple[EvaluationJob, ...]:
    """Select supported-view positives without tuning against rejected geometry."""
    jobs: list[EvaluationJob] = []
    for clip in manifest.clips:
        if clip.disposition not in corpus.POSITIVE_DISPOSITIONS:
            continue
        if clip.hitting_region is None:
            message = f"positive clip {clip.clip_id} has no hitting region"
            raise ValueError(message)
        jobs.append(
            EvaluationJob(
                clip=clip,
                media_path=media_root / Path(*clip.media_path.parts),
                output_directory=output_root / clip.clip_id,
            )
        )
    return tuple(jobs)


def _run(command: Sequence[str], *, capture_output: bool = False) -> str:
    completed = subprocess.run(
        command,
        check=True,
        text=True,
        capture_output=capture_output,
    )
    return completed.stdout if capture_output else ""


def _required_label(value: int | None, name: str, clip_id: str) -> int:
    if value is None:
        message = f"positive clip {clip_id} is missing {name}"
        raise ValueError(message)
    return value


def _replay_arguments(job: EvaluationJob, executable: Path, startup_budget_ms: int) -> list[str]:
    labels = job.clip.labels
    safe_arm_ms = _required_label(labels.safe_arm_start_ms, "safe arm", job.clip.clip_id)
    preferred_arm_ms = _required_label(labels.preferred_arm_ms, "preferred arm", job.clip.clip_id)
    takeaway_ms = _required_label(labels.takeaway_ms, "takeaway", job.clip.clip_id)
    arguments = [
        str(executable),
        f"--observations={job.output_directory / 'observations.csv'}",
        f"--safe-arm-start-ms={safe_arm_ms}",
        f"--preferred-arm-ms={preferred_arm_ms}",
        f"--takeaway-ms={takeaway_ms}",
        f"--startup-budget-ms={startup_budget_ms}",
    ]
    if labels.must_not_arm:
        intervals = ",".join(
            f"{interval.start_ms}:{interval.end_ms}" for interval in labels.must_not_arm
        )
        arguments.append(f"--must-not-arm-ms={intervals}")
    return arguments


def parse_replay_result(rendered: str, clip_id: str) -> dict[str, object]:
    """Validate the narrow JSON contract emitted by the shared Java replay."""
    decoded = cast("object", json.loads(rendered))
    if not isinstance(decoded, dict):
        message = f"{clip_id} replay emitted an invalid result"
        raise TypeError(message)
    value = cast("dict[str, object]", decoded)
    if value.get("schema_version") != 1:
        message = f"{clip_id} replay emitted an invalid result"
        raise ValueError(message)
    passed = value.get("passed")
    if not isinstance(passed, bool):
        message = f"{clip_id} replay result has no boolean passed field"
        raise TypeError(message)
    return value


def _arm_milliseconds(result: dict[str, object], fallback_ms: int) -> int:
    encoded = result.get("arm_request_ns")
    if encoded is None:
        return fallback_ms
    if not isinstance(encoded, str) or not encoded.isdigit():
        message = "replay arm_request_ns must be a decimal string or null"
        raise ValueError(message)
    nanoseconds = int(encoded)
    if nanoseconds % NANOS_PER_MILLISECOND != 0:
        message = "replay arm_request_ns is not millisecond aligned"
        raise ValueError(message)
    return nanoseconds // NANOS_PER_MILLISECOND


def evaluate_job(
    job: EvaluationJob, executables: Executables, startup_budget_ms: int
) -> dict[str, object]:
    """Execute all stages for one clip and return its aggregate evidence."""
    clip = job.clip
    region = clip.hitting_region
    if region is None:
        message = f"positive clip {clip.clip_id} has no hitting region"
        raise ValueError(message)
    job.output_directory.mkdir()
    landmarks_ndjson = job.output_directory / "landmarks.ndjson"
    landmarks_csv = job.output_directory / "landmarks.csv"
    observations_csv = job.output_directory / "observations.csv"
    annotated_video = job.output_directory / "annotated.mp4"

    _run(
        [
            str(executables.landmarker),
            f"--input={job.media_path}",
            f"--output={landmarks_ndjson}",
        ]
    )
    _run(
        [
            str(executables.landmark_csv),
            f"--input={landmarks_ndjson}",
            f"--output={landmarks_csv}",
        ]
    )
    _run(
        [
            str(executables.observation_adapter),
            f"--landmarks={landmarks_csv}",
            f"--observations={observations_csv}",
            f"--hitting-region={region.encoded()}",
            f"--projection={clip.view}",
        ]
    )
    replay = parse_replay_result(
        _run(_replay_arguments(job, executables.replay, startup_budget_ms), capture_output=True),
        clip.clip_id,
    )
    replay_path = job.output_directory / "replay.json"
    replay_path.write_text(json.dumps(replay, sort_keys=True) + "\n", encoding="utf-8")
    takeaway_ms = _required_label(clip.labels.takeaway_ms, "takeaway", clip.clip_id)
    _run(
        [
            str(executables.annotator),
            f"--input={job.media_path}",
            f"--landmarks={landmarks_ndjson}",
            f"--observations={observations_csv}",
            f"--output={annotated_video}",
            f"--arm-ms={_arm_milliseconds(replay, clip.duration_ms + 1)}",
            f"--startup-budget-ms={startup_budget_ms}",
            f"--takeaway-ms={takeaway_ms}",
            f"--hitting-region={region.encoded()}",
        ]
    )
    return {
        "id": clip.clip_id,
        "split": clip.split,
        "disposition": clip.disposition,
        "view": clip.view,
        "hitting_region": dataclasses.astuple(region),
        "human_safe_arm_start_ms": clip.labels.safe_arm_start_ms,
        "human_preferred_arm_ms": clip.labels.preferred_arm_ms,
        "human_takeaway_ms": clip.labels.takeaway_ms,
        "replay": replay,
        "artifacts": {
            "landmarks": f"{clip.clip_id}/landmarks.ndjson",
            "observations": f"{clip.clip_id}/observations.csv",
            "replay": f"{clip.clip_id}/replay.json",
            "annotated_video": f"{clip.clip_id}/annotated.mp4",
        },
    }


def aggregate(
    manifest: corpus.Corpus, results: Sequence[dict[str, object]], startup_budget_ms: int
) -> dict[str, object]:
    """Build one stable report without hiding challenge failures."""
    passed = sum(
        1 for result in results if cast("dict[str, object]", result["replay"])["passed"] is True
    )
    evaluated_ids = {cast("str", result["id"]) for result in results}
    excluded = [
        {
            "id": clip.clip_id,
            "disposition": clip.disposition,
            "view": clip.view,
            "reference_arm_ms": clip.labels.reference_arm_ms,
        }
        for clip in manifest.clips
        if clip.clip_id not in evaluated_ids
    ]
    return {
        "schema_version": 1,
        "sample_period_ms": manifest.sample_period_ms,
        "startup_budget_ms": startup_budget_ms,
        "evaluated_clip_count": len(results),
        "passed_clip_count": passed,
        "failed_clip_count": len(results) - passed,
        "results": list(results),
        "excluded": excluded,
    }


def _absolute_path(value: str) -> Path:
    path = Path(value)
    if not path.is_absolute():
        message = "Bazel-run corpus paths must be absolute"
        raise argparse.ArgumentTypeError(message)
    return path


def main(arguments: Sequence[str] | None = None) -> int:
    """Run inference, shared feature extraction, replay, and annotation."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=_absolute_path, required=True)
    parser.add_argument("--media-root", type=_absolute_path, required=True)
    parser.add_argument("--output-root", type=_absolute_path, required=True)
    parser.add_argument("--startup-budget-ms", type=int, default=800)
    options = parser.parse_args(arguments)
    manifest_path = cast("Path", options.manifest)
    media_root = cast("Path", options.media_root)
    output_root = cast("Path", options.output_root)
    startup_budget_ms = cast("int", options.startup_budget_ms)
    if startup_budget_ms < 0:
        parser.error("startup-budget-ms cannot be negative")
    parsed = corpus.parse_manifest(manifest_path.read_text(encoding="utf-8"))
    corpus.validate_media(parsed, media_root)
    output_root.mkdir(parents=True)
    jobs = build_jobs(parsed, media_root, output_root)
    executables = Executables.from_runfiles()
    results = [evaluate_job(job, executables, startup_budget_ms) for job in jobs]
    report = aggregate(parsed, results, startup_budget_ms)
    report_path = output_root / "report.json"
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
