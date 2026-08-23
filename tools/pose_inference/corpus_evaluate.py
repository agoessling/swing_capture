"""Run the Bazel-owned pose pipeline over every supported corpus clip."""

from __future__ import annotations

import argparse
import dataclasses
import enum
import json
import subprocess
from pathlib import Path
from typing import TYPE_CHECKING, Protocol, cast

from python.runfiles import runfiles

from tools.pose_inference import corpus

if TYPE_CHECKING:
    from collections.abc import Sequence


NANOS_PER_MILLISECOND = 1_000_000


class RunfilesResolver(Protocol):
    """Typed subset of Bazel's dynamically typed runfiles resolver."""

    def Rlocation(self, logical_path: str) -> str | None:  # noqa: N802
        """Resolve one logical runfile path."""
        ...


class ModelVariant(enum.StrEnum):
    """Closed set of official MediaPipe Pose Landmarker model variants."""

    LITE = "lite"
    FULL = "full"
    HEAVY = "heavy"


@dataclasses.dataclass(frozen=True)
class ModelAssets:
    """Runfile-resolved official Pose Landmarker model assets."""

    lite: Path
    full: Path
    heavy: Path

    def select(self, variant: ModelVariant) -> Path:
        """Return the pinned asset for a validated model variant."""
        return {
            ModelVariant.LITE: self.lite,
            ModelVariant.FULL: self.full,
            ModelVariant.HEAVY: self.heavy,
        }[variant]


@dataclasses.dataclass(frozen=True)
class Executables:
    """Runfile-resolved Bazel stage binaries."""

    landmarker: Path
    landmark_csv: Path
    observation_adapter: Path
    replay: Path
    annotator: Path
    models: ModelAssets

    @classmethod
    def from_runfiles(cls) -> Executables:
        """Resolve every stage without relying on a developer checkout layout."""
        resolver = runfiles.Create()
        if resolver is None:
            message = "Bazel runfiles are unavailable"
            raise RuntimeError(message)

        return cls.from_resolver(cast("RunfilesResolver", cast("object", resolver)))

    @classmethod
    def from_resolver(cls, resolver: RunfilesResolver) -> Executables:
        """Resolve every stage through an injected, typed runfiles resolver."""

        def resolve(logical_path: str) -> Path:
            resolved = resolver.Rlocation(f"_main/{logical_path}")
            if not resolved:
                message = f"missing Bazel runfile: {logical_path}"
                raise RuntimeError(message)
            return Path(resolved)

        def resolve_external(logical_path: str) -> Path:
            resolved = resolver.Rlocation(logical_path)
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
            models=ModelAssets(
                lite=resolve_external("pose_landmarker_lite_task/file/pose_landmarker_lite.task"),
                full=resolve_external("pose_landmarker_full_task/file/pose_landmarker_full.task"),
                heavy=resolve_external(
                    "pose_landmarker_heavy_task/file/pose_landmarker_heavy.task"
                ),
            ),
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


def landmarker_arguments(job: EvaluationJob, executable: Path, model_asset: Path) -> list[str]:
    """Build the inference command with an explicit Bazel-owned model asset."""
    return [
        str(executable),
        f"--input={job.media_path}",
        f"--model={model_asset}",
        f"--output={job.output_directory / 'landmarks.ndjson'}",
    ]


def observation_adapter_arguments(job: EvaluationJob, executable: Path) -> list[str]:
    """Build ROI-free feature extraction arguments for one corpus clip."""
    return [
        str(executable),
        f"--landmarks={job.output_directory / 'landmarks.csv'}",
        f"--observations={job.output_directory / 'observations.csv'}",
        f"--hitting-region={corpus.FULL_FRAME_HITTING_REGION.encoded()}",
        f"--projection={job.clip.view}",
    ]


@dataclasses.dataclass(frozen=True)
class _ReplayEvidence:
    observation_count: int
    arm_count: int
    arm_ns: int | None
    offset_ns: int | None
    ready_ns: int | None
    lead_ns: int | None
    before_safe: bool
    forbidden: bool
    ready: bool
    passed: bool
    outcome: str


def parse_replay_result(rendered: str, clip_id: str) -> dict[str, object]:
    """Validate the narrow JSON contract emitted by the shared Java replay."""
    decoded = cast("object", json.loads(rendered))
    if not isinstance(decoded, dict):
        message = f"{clip_id} replay emitted an invalid result"
        raise TypeError(message)
    value = cast("dict[str, object]", decoded)
    _validate_replay_shape(value, clip_id)
    evidence = _parse_replay_evidence(value, clip_id)
    _validate_replay_timing(evidence, clip_id)
    expected_outcome = _expected_replay_outcome(evidence)
    if evidence.outcome != expected_outcome or evidence.passed != evidence.outcome.startswith(
        "acceptable"
    ):
        message = f"{clip_id} replay outcome contradicts its timing evidence"
        raise ValueError(message)
    return value


def _validate_replay_shape(value: dict[str, object], clip_id: str) -> None:
    expected_fields = {
        "schema_version",
        "observation_count",
        "arm_request_ns",
        "arm_offset_from_preferred_ns",
        "high_speed_ready_ns",
        "ready_lead_before_takeaway_ns",
        "armed_before_safe_window",
        "armed_in_forbidden_interval",
        "ready_by_takeaway",
        "passed",
        "outcome",
        "arm_request_count",
        "final_state",
    }
    if set(value) != expected_fields or value.get("schema_version") != 1:
        message = f"{clip_id} replay emitted an invalid result"
        raise ValueError(message)
    if value["final_state"] not in {
        "watching",
        "qualifying",
        "arm_requested",
        "waiting_for_clear",
    }:
        message = f"{clip_id} replay has an unsupported final state"
        raise ValueError(message)


def _parse_replay_evidence(value: dict[str, object], clip_id: str) -> _ReplayEvidence:
    outcome = value["outcome"]
    if not isinstance(outcome, str) or outcome not in {
        "acceptable",
        "acceptable_early",
        "no_arm_request",
        "unsafe_early_arm",
        "forbidden_arm",
        "not_ready_by_takeaway",
    }:
        message = f"{clip_id} replay has an unsupported outcome"
        raise ValueError(message)
    return _ReplayEvidence(
        observation_count=_nonnegative_integer(value["observation_count"], "observation_count"),
        arm_count=_nonnegative_integer(value["arm_request_count"], "arm_request_count"),
        arm_ns=_optional_decimal(value["arm_request_ns"], "arm_request_ns", signed=False),
        offset_ns=_optional_decimal(
            value["arm_offset_from_preferred_ns"],
            "arm_offset_from_preferred_ns",
            signed=True,
        ),
        ready_ns=_optional_decimal(
            value["high_speed_ready_ns"], "high_speed_ready_ns", signed=False
        ),
        lead_ns=_optional_decimal(
            value["ready_lead_before_takeaway_ns"],
            "ready_lead_before_takeaway_ns",
            signed=True,
        ),
        before_safe=_boolean(value["armed_before_safe_window"], "armed_before_safe_window"),
        forbidden=_boolean(value["armed_in_forbidden_interval"], "armed_in_forbidden_interval"),
        ready=_boolean(value["ready_by_takeaway"], "ready_by_takeaway"),
        passed=_boolean(value["passed"], "passed"),
        outcome=outcome,
    )


def _validate_replay_timing(evidence: _ReplayEvidence, clip_id: str) -> None:
    if evidence.arm_count > evidence.observation_count:
        message = f"{clip_id} replay has more arm requests than observations"
        raise ValueError(message)
    has_arm = evidence.arm_count > 0
    if has_arm != (evidence.arm_ns is not None):
        message = f"{clip_id} replay arm count contradicts its first arm timestamp"
        raise ValueError(message)
    timing = (evidence.offset_ns, evidence.ready_ns, evidence.lead_ns)
    if any(value is not None for value in timing) != has_arm or (
        has_arm and any(value is None for value in timing)
    ):
        message = f"{clip_id} replay arm timing fields are incomplete"
        raise ValueError(message)
    if not has_arm and (evidence.before_safe or evidence.forbidden or evidence.ready):
        message = f"{clip_id} replay without an arm retains arm evidence"
        raise ValueError(message)
    if has_arm and (
        evidence.arm_ns is None
        or evidence.ready_ns is None
        or evidence.lead_ns is None
        or evidence.ready_ns < evidence.arm_ns
        or evidence.ready != (evidence.lead_ns >= 0)
    ):
        message = f"{clip_id} replay timing contradicts readiness"
        raise ValueError(message)


def _expected_replay_outcome(evidence: _ReplayEvidence) -> str:
    if evidence.arm_count == 0:
        return "no_arm_request"
    if evidence.forbidden:
        return "forbidden_arm"
    if evidence.before_safe:
        return "unsafe_early_arm"
    if not evidence.ready:
        return "not_ready_by_takeaway"
    if evidence.offset_ns is not None and evidence.offset_ns < 0:
        return "acceptable_early"
    return "acceptable"


def _nonnegative_integer(value: object, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        message = f"replay {name} must be an integer"
        raise TypeError(message)
    if value < 0:
        message = f"replay {name} cannot be negative"
        raise ValueError(message)
    return value


def _optional_decimal(value: object, name: str, *, signed: bool) -> int | None:
    if value is None:
        return None
    if not isinstance(value, str):
        message = f"replay {name} must be a decimal string or null"
        raise TypeError(message)
    digits = value[1:] if signed and value.startswith("-") else value
    if not digits.isdigit():
        message = f"replay {name} must be a decimal string or null"
        raise ValueError(message)
    return int(value)


def _boolean(value: object, name: str) -> bool:
    if not isinstance(value, bool):
        message = f"replay {name} must be boolean"
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
    job: EvaluationJob,
    executables: Executables,
    startup_budget_ms: int,
    model_variant: ModelVariant = ModelVariant.LITE,
) -> dict[str, object]:
    """Execute all stages for one clip and return its aggregate evidence."""
    clip = job.clip
    region = corpus.FULL_FRAME_HITTING_REGION
    job.output_directory.mkdir()
    landmarks_ndjson = job.output_directory / "landmarks.ndjson"
    landmarks_csv = job.output_directory / "landmarks.csv"
    observations_csv = job.output_directory / "observations.csv"
    annotated_video = job.output_directory / "annotated.mp4"

    _run(
        landmarker_arguments(job, executables.landmarker, executables.models.select(model_variant))
    )
    _run(
        [
            str(executables.landmark_csv),
            f"--input={landmarks_ndjson}",
            f"--output={landmarks_csv}",
        ]
    )
    _run(observation_adapter_arguments(job, executables.observation_adapter))
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
        "legacy_manifest_hitting_region": (
            None if clip.hitting_region is None else dataclasses.astuple(clip.hitting_region)
        ),
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
    manifest: corpus.Corpus,
    results: Sequence[dict[str, object]],
    startup_budget_ms: int,
    model_variant: ModelVariant = ModelVariant.LITE,
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
        "startup_budget_source": "required_command_line",
        "model_variant": model_variant.value,
        "spatial_evaluation_policy": "full_frame",
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
    parser.add_argument(
        "--startup-budget-ms",
        type=int,
        required=True,
        help=(
            "measured arm-command-to-usable-frame budget; required so an "
            "evaluation cannot silently inherit a stale device assumption"
        ),
    )
    parser.add_argument(
        "--model-variant",
        choices=tuple(variant.value for variant in ModelVariant),
        default=ModelVariant.LITE.value,
        help="official Bazel-pinned Pose Landmarker model variant (default: lite)",
    )
    options = parser.parse_args(arguments)
    manifest_path = cast("Path", options.manifest)
    media_root = cast("Path", options.media_root)
    output_root = cast("Path", options.output_root)
    startup_budget_ms = cast("int", options.startup_budget_ms)
    model_variant = ModelVariant(cast("str", options.model_variant))
    if startup_budget_ms < 0:
        parser.error("startup-budget-ms cannot be negative")
    parsed = corpus.parse_manifest(manifest_path.read_text(encoding="utf-8"))
    corpus.validate_media(parsed, media_root)
    output_root.mkdir(parents=True)
    jobs = build_jobs(parsed, media_root, output_root)
    executables = Executables.from_runfiles()
    results = [evaluate_job(job, executables, startup_budget_ms, model_variant) for job in jobs]
    report = aggregate(parsed, results, startup_budget_ms, model_variant)
    report_path = output_root / "report.json"
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
