"""Same-holdout baseline-versus-candidate audio policy enforcement."""

# Validation failures stay at their evidence checks so reports retain precise diagnostics.
# ruff: noqa: EM101, EM102, TRY003

from __future__ import annotations

from typing import cast

REPORT_TYPE = "audio_detector_holdout_comparison"
PRODUCTION_ALGORITHM_ID = "android-impact-detector-v1"
CONTINUOUS_CANDIDATE_MATCHING = "one_to_one_maximum_cardinality_v1"


def compare_reports(  # noqa: C901, PLR0912 - one cross-report policy contract.
    baseline: dict[str, object], candidate: dict[str, object]
) -> dict[str, object]:
    """Require an absolutely passing candidate with no production-baseline regression."""
    _evaluation_report(baseline, "baseline")
    _evaluation_report(candidate, "candidate")
    if baseline["corpus_id"] != candidate["corpus_id"]:
        raise ValueError("baseline and candidate corpus IDs differ")
    if baseline["holdout_recording_ids"] != candidate["holdout_recording_ids"]:
        raise ValueError("baseline and candidate holdout recording IDs differ")
    if baseline["holdout_evidence_sha256"] != candidate["holdout_evidence_sha256"]:
        raise ValueError("baseline and candidate holdout evidence SHA-256 differ")
    if baseline["evaluation_policy"] != candidate["evaluation_policy"]:
        raise ValueError("baseline and candidate evaluation policies differ")
    _complete_leakage_controls(baseline, "baseline")
    _complete_leakage_controls(candidate, "candidate")
    if baseline["leakage_controls"] != candidate["leakage_controls"]:
        raise ValueError("baseline and candidate leakage-control provenance differs")
    baseline_detector = _object(baseline["detector"], "baseline.detector")
    candidate_detector = _object(candidate["detector"], "candidate.detector")
    if baseline_detector.get("algorithm_id") != PRODUCTION_ALGORITHM_ID:
        raise ValueError("baseline is not the production Android audio detector")
    if (
        baseline_detector.get("implementation_target")
        != "//tools/field_evidence:production_audio_detector_replay"
    ):
        raise ValueError("baseline does not use the production Android replay target")
    if baseline["policy_lock_sha256"] == candidate["policy_lock_sha256"]:
        raise ValueError("candidate must use a distinct frozen policy lock")
    if baseline_detector == candidate_detector:
        raise ValueError("candidate detector is identical to the production baseline")

    baseline_generation = _object(baseline["candidate_generation"], "baseline.candidate_generation")
    candidate_generation = _object(
        candidate["candidate_generation"], "candidate.candidate_generation"
    )
    baseline_lifecycle = _object(baseline["lifecycle"], "baseline.lifecycle")
    candidate_lifecycle = _object(candidate["lifecycle"], "candidate.lifecycle")
    for key in ("target_count", "quiet_target_count"):
        if _metric(baseline_generation, key, "baseline") != _metric(
            candidate_generation, key, "candidate"
        ):
            raise ValueError(f"baseline and candidate {key} denominators differ")
    for key in ("target_count", "reviewed_ms"):
        if _metric(baseline_lifecycle, key, "baseline") != _metric(
            candidate_lifecycle, key, "candidate"
        ):
            raise ValueError(f"baseline and candidate lifecycle {key} denominators differ")

    comparisons = {
        "target_recalled": _nondecreasing(
            baseline_generation, candidate_generation, "target_recalled"
        ),
        "quiet_target_recalled": _nondecreasing(
            baseline_generation, candidate_generation, "quiet_target_recalled"
        ),
        "non_target_candidate_count": _nonincreasing(
            baseline_generation, candidate_generation, "non_target_candidate_count"
        ),
        "captured_target_count": _nondecreasing(
            baseline_lifecycle, candidate_lifecycle, "captured_target_count"
        ),
        "false_terminal_attempt_count": _nonincreasing(
            baseline_lifecycle, candidate_lifecycle, "false_terminal_attempt_count"
        ),
        "high_speed_duty_ppm": _nonincreasing(
            baseline_lifecycle, candidate_lifecycle, "high_speed_duty_ppm"
        ),
    }
    baseline_categories = _object(
        baseline_generation.get("labeled_negative_candidates_by_category"),
        "baseline.labeled_negative_candidates_by_category",
    )
    candidate_categories = _object(
        candidate_generation.get("labeled_negative_candidates_by_category"),
        "candidate.labeled_negative_candidates_by_category",
    )
    if set(baseline_categories) != set(candidate_categories):
        raise ValueError("baseline and candidate negative-category coverage differs")
    for category in sorted(baseline_categories):
        comparisons[f"negative_category:{category}"] = _nonincreasing(
            baseline_categories, candidate_categories, category
        )
    candidate_passed = candidate.get("passed") is True
    no_regressions = all(comparison["no_regression"] is True for comparison in comparisons.values())
    strict_improvement = any(
        comparison["strict_improvement"] is True for comparison in comparisons.values()
    )
    checks = {
        "candidate_absolute_acceptance": candidate_passed,
        "no_baseline_regressions": no_regressions,
        "strict_improvement": strict_improvement,
    }
    return {
        "schema_version": 1,
        "report_type": REPORT_TYPE,
        "passed": all(checks.values()),
        "corpus_id": baseline["corpus_id"],
        "holdout_evidence_sha256": baseline["holdout_evidence_sha256"],
        "holdout_recording_ids": baseline["holdout_recording_ids"],
        "baseline_policy_id": baseline["policy_id"],
        "baseline_policy_lock_sha256": baseline["policy_lock_sha256"],
        "candidate_policy_id": candidate["policy_id"],
        "candidate_policy_lock_sha256": candidate["policy_lock_sha256"],
        "checks": checks,
        "comparisons": comparisons,
    }


def _evaluation_report(report: dict[str, object], label: str) -> None:
    required = {
        "schema_version",
        "report_type",
        "passed",
        "corpus_id",
        "holdout_evidence_sha256",
        "holdout_recording_ids",
        "policy_id",
        "policy_lock_sha256",
        "detector",
        "evaluation_policy",
        "leakage_controls",
        "checks",
        "candidate_generation",
        "lifecycle",
    }
    if set(report) != required:
        raise ValueError(f"{label} evaluation report keys differ")
    if (
        report["schema_version"] != 1
        or report["report_type"] != "audio_detector_holdout_evaluation"
    ):
        raise ValueError(f"{label} is not a supported audio holdout evaluation")
    if not isinstance(report["passed"], bool):
        raise TypeError(f"{label}.passed must be Boolean")
    evaluation = _object(report["evaluation_policy"], f"{label}.evaluation_policy")
    if evaluation.get("continuous_candidate_matching") != CONTINUOUS_CANDIDATE_MATCHING:
        raise ValueError(f"{label} uses unsupported continuous-candidate matching semantics")
    for key in ("corpus_id", "policy_id", "policy_lock_sha256"):
        if not isinstance(report[key], str) or not report[key]:
            raise TypeError(f"{label}.{key} must be nonempty text")
    holdouts = report["holdout_recording_ids"]
    holdout_items = cast("list[object]", holdouts) if isinstance(holdouts, list) else []
    if (
        not isinstance(holdouts, list)
        or not holdouts
        or not all(isinstance(item, str) and item for item in holdout_items)
    ):
        raise TypeError(f"{label}.holdout_recording_ids must be a nonempty string array")


def _complete_leakage_controls(report: dict[str, object], label: str) -> None:
    controls = _object(report["leakage_controls"], f"{label}.leakage_controls")
    required_true = {
        "policy_frozen_before_all_captures",
        "phone_view_swapped",
        "complete_timeline_review",
        "assets_hash_verified",
        "all_qualifying_holdouts_scored",
        "single_frozen_policy",
    }
    if any(controls.get(key) is not True for key in required_true):
        raise ValueError(f"{label} leakage controls are incomplete")


def _nondecreasing(
    baseline: dict[str, object], candidate: dict[str, object], key: str
) -> dict[str, object]:
    before = _metric(baseline, key, "baseline")
    after = _metric(candidate, key, "candidate")
    return {
        "baseline": before,
        "candidate": after,
        "direction": "higher_is_better",
        "no_regression": after >= before,
        "strict_improvement": after > before,
    }


def _nonincreasing(
    baseline: dict[str, object], candidate: dict[str, object], key: str
) -> dict[str, object]:
    before = _metric(baseline, key, "baseline")
    after = _metric(candidate, key, "candidate")
    return {
        "baseline": before,
        "candidate": after,
        "direction": "lower_is_better",
        "no_regression": after <= before,
        "strict_improvement": after < before,
    }


def _object(value: object, label: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise TypeError(f"{label} must be an object")
    mapping = cast("dict[object, object]", value)
    if not all(isinstance(key, str) for key in mapping):
        raise TypeError(f"{label} must be an object")
    return cast("dict[str, object]", mapping)


def _metric(value: dict[str, object], key: str, label: str) -> int:
    metric = value.get(key)
    if not isinstance(metric, int) or isinstance(metric, bool) or metric < 0:
        raise TypeError(f"{label}.{key} must be a nonnegative integer")
    return metric
