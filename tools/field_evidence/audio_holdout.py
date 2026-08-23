"""Leakage-resistant scoring for audio-detector holdout predictions."""

from __future__ import annotations

import dataclasses
import datetime
import hashlib
import itertools
import json
from pathlib import Path, PurePath
from typing import TYPE_CHECKING, cast

from tools.field_evidence import manifest

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence


SCHEMA_VERSION = 1
REPORT_TYPE = "audio_detector_holdout_predictions"
LOCK_TYPE = "audio_detector_policy_lock"
CONTINUOUS_CANDIDATE_MATCHING = "one_to_one_maximum_cardinality_v1"
SHA256_HEX_LENGTH = 64


@dataclasses.dataclass(frozen=True)
class Candidate:
    """One detector candidate with its estimated strike and online decision times."""

    strike_ms: int
    decision_ms: int


@dataclasses.dataclass(frozen=True)
class ArmedReplay:
    """Detector output from one reset-at-arm lifecycle replay."""

    attempt_id: str
    arm_ms: int
    evaluation_end_ms: int
    candidates: tuple[Candidate, ...]


@dataclasses.dataclass(frozen=True)
class StreamPredictions:
    """Continuous and armed predictions for one camera's audio stream."""

    continuous_candidates: tuple[Candidate, ...]
    armed_replays: Mapping[str, ArmedReplay]


@dataclasses.dataclass(frozen=True)
class SessionPredictions:
    """Predictions for both streams in one coordinated field recording."""

    recording_id: str
    streams: Mapping[str, StreamPredictions]


@dataclasses.dataclass(frozen=True)
class EvaluationPolicy:
    """Frozen acceptance thresholds for the holdout evaluation."""

    leader_role: str
    target_tolerance_ms: int
    audio_ready_delay_ms: int
    video_ready_delay_ms: int
    maximum_armed_ms: int
    post_terminal_ms: int
    rearm_delay_ms: int
    retained_history_ms: int
    minimum_target_recall_ppm: int
    minimum_quiet_target_recall_ppm: int
    maximum_negative_continuous_candidates: int
    maximum_false_terminal_attempts: int
    minimum_lifecycle_capture_ppm: int
    maximum_high_speed_duty_ppm: int


@dataclasses.dataclass(frozen=True)
class PolicyLock:
    """Immutable detector configuration and holdout evaluation policy."""

    policy_id: str
    frozen_at_utc: datetime.datetime
    development_recording_ids: tuple[str, ...]
    detector: Mapping[str, object]
    evaluation: EvaluationPolicy
    canonical_sha256: str


def _object(value: object, label: str) -> dict[str, object]:
    if not isinstance(value, dict):
        message = f"{label} must be an object with string keys"
        raise TypeError(message)
    untyped = cast("dict[object, object]", value)
    if not all(isinstance(key, str) for key in untyped):
        message = f"{label} must be an object with string keys"
        raise TypeError(message)
    return cast("dict[str, object]", untyped)


def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            message = f"duplicate JSON field {key!r}"
            raise ValueError(message)
        result[key] = value
    return result


def _decode_json(text: str) -> object:
    return cast("object", json.loads(text, object_pairs_hook=_reject_duplicate_keys))


def _array(value: object, label: str) -> list[object]:
    if not isinstance(value, list):
        message = f"{label} must be an array"
        raise TypeError(message)
    return cast("list[object]", value)


def _exact_keys(value: Mapping[str, object], expected: set[str], label: str) -> None:
    if set(value) != expected:
        message = (
            f"{label} keys differ: missing={sorted(expected - set(value))} "
            f"unknown={sorted(set(value) - expected)}"
        )
        raise ValueError(message)


def _integer(value: object, label: str, *, minimum: int = 0, maximum: int | None = None) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        message = f"{label} must be an integer"
        raise TypeError(message)
    if value < minimum or (maximum is not None and value > maximum):
        message = f"{label} is outside the supported range"
        raise ValueError(message)
    return value


def _text(value: object, label: str) -> str:
    if not isinstance(value, str) or not value.strip():
        message = f"{label} must be nonempty text"
        raise TypeError(message)
    return value


def _timestamp(value: object, label: str) -> datetime.datetime:
    encoded = _text(value, label)
    try:
        parsed = datetime.datetime.fromisoformat(encoded.replace("Z", "+00:00"))
    except ValueError as error:
        message = f"{label} must be a timezone-aware RFC 3339 timestamp"
        raise ValueError(message) from error
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        message = f"{label} must be a timezone-aware RFC 3339 timestamp"
        raise ValueError(message)
    return parsed.astimezone(datetime.UTC)


def _canonical_json(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()


def _json_value(value: object) -> object:
    if dataclasses.is_dataclass(value) and not isinstance(value, type):
        return {
            field.name: _json_value(cast("object", getattr(value, field.name)))
            for field in dataclasses.fields(value)
        }
    if isinstance(value, PurePath):
        return str(value)
    if isinstance(value, dict):
        untyped = cast("dict[object, object]", value)
        return {str(key): _json_value(item) for key, item in untyped.items()}
    if isinstance(value, (tuple, list)):
        return [_json_value(item) for item in cast("Sequence[object]", value)]
    return value


def _holdout_evidence_sha256(corpus: manifest.Corpus, sessions: Sequence[manifest.Session]) -> str:
    evidence_identity = {
        "corpus_id": corpus.corpus_id,
        "development_recording_ids": corpus.development_recording_ids,
        "development_device_assignment": corpus.development_device_assignment,
        "holdout_sessions": sessions,
    }
    return hashlib.sha256(_canonical_json(_json_value(evidence_identity))).hexdigest()


def _parse_evaluation(value: object) -> EvaluationPolicy:
    encoded = _object(value, "policy_lock.evaluation")
    expected = {
        "leader_role",
        "target_tolerance_ms",
        "audio_ready_delay_ms",
        "video_ready_delay_ms",
        "maximum_armed_ms",
        "post_terminal_ms",
        "rearm_delay_ms",
        "retained_history_ms",
        "minimum_target_recall_ppm",
        "minimum_quiet_target_recall_ppm",
        "maximum_negative_continuous_candidates",
        "maximum_false_terminal_attempts",
        "minimum_lifecycle_capture_ppm",
        "maximum_high_speed_duty_ppm",
    }
    _exact_keys(encoded, expected, "policy_lock.evaluation")
    leader = _text(encoded["leader_role"], "policy_lock.evaluation.leader_role")
    if leader not in manifest.ROLES:
        message = "policy_lock.evaluation.leader_role is unsupported"
        raise ValueError(message)
    result = EvaluationPolicy(
        leader_role=leader,
        target_tolerance_ms=_integer(
            encoded["target_tolerance_ms"], "policy_lock.evaluation.target_tolerance_ms"
        ),
        audio_ready_delay_ms=_integer(
            encoded["audio_ready_delay_ms"], "policy_lock.evaluation.audio_ready_delay_ms"
        ),
        video_ready_delay_ms=_integer(
            encoded["video_ready_delay_ms"], "policy_lock.evaluation.video_ready_delay_ms"
        ),
        maximum_armed_ms=_integer(
            encoded["maximum_armed_ms"], "policy_lock.evaluation.maximum_armed_ms", minimum=1
        ),
        post_terminal_ms=_integer(
            encoded["post_terminal_ms"], "policy_lock.evaluation.post_terminal_ms"
        ),
        rearm_delay_ms=_integer(encoded["rearm_delay_ms"], "policy_lock.evaluation.rearm_delay_ms"),
        retained_history_ms=_integer(
            encoded["retained_history_ms"], "policy_lock.evaluation.retained_history_ms"
        ),
        minimum_target_recall_ppm=_integer(
            encoded["minimum_target_recall_ppm"],
            "policy_lock.evaluation.minimum_target_recall_ppm",
            maximum=1_000_000,
        ),
        minimum_quiet_target_recall_ppm=_integer(
            encoded["minimum_quiet_target_recall_ppm"],
            "policy_lock.evaluation.minimum_quiet_target_recall_ppm",
            maximum=1_000_000,
        ),
        maximum_negative_continuous_candidates=_integer(
            encoded["maximum_negative_continuous_candidates"],
            "policy_lock.evaluation.maximum_negative_continuous_candidates",
        ),
        maximum_false_terminal_attempts=_integer(
            encoded["maximum_false_terminal_attempts"],
            "policy_lock.evaluation.maximum_false_terminal_attempts",
        ),
        minimum_lifecycle_capture_ppm=_integer(
            encoded["minimum_lifecycle_capture_ppm"],
            "policy_lock.evaluation.minimum_lifecycle_capture_ppm",
            maximum=1_000_000,
        ),
        maximum_high_speed_duty_ppm=_integer(
            encoded["maximum_high_speed_duty_ppm"],
            "policy_lock.evaluation.maximum_high_speed_duty_ppm",
            maximum=1_000_000,
        ),
    )
    if result.video_ready_delay_ms > result.audio_ready_delay_ms:
        message = "video readiness cannot follow audio trigger readiness"
        raise ValueError(message)
    if result.retained_history_ms <= 0:
        message = "retained_history_ms must be positive"
        raise ValueError(message)
    return result


def parse_policy_lock(text: str) -> PolicyLock:
    """Parse one detector policy selected before the holdout was acquired."""
    raw = _decode_json(text)
    encoded = _object(raw, "policy_lock")
    _exact_keys(
        encoded,
        {
            "schema_version",
            "report_type",
            "policy_id",
            "frozen_at_utc",
            "development_recording_ids",
            "detector",
            "evaluation",
        },
        "policy_lock",
    )
    if encoded["schema_version"] != SCHEMA_VERSION or encoded["report_type"] != LOCK_TYPE:
        message = "unsupported audio detector policy lock"
        raise ValueError(message)
    development_ids = tuple(
        _text(item, f"policy_lock.development_recording_ids[{index}]")
        for index, item in enumerate(
            _array(encoded["development_recording_ids"], "policy_lock.development_recording_ids")
        )
    )
    if not development_ids or len(development_ids) != len(set(development_ids)):
        message = "policy lock development recording IDs must be nonempty and unique"
        raise ValueError(message)
    detector = _object(encoded["detector"], "policy_lock.detector")
    _exact_keys(
        detector,
        {
            "algorithm_id",
            "implementation_target",
            "implementation_sha256",
            "config_by_device",
        },
        "policy_lock.detector",
    )
    _text(detector["algorithm_id"], "policy_lock.detector.algorithm_id")
    _text(detector["implementation_target"], "policy_lock.detector.implementation_target")
    implementation_sha256 = _text(
        detector["implementation_sha256"], "policy_lock.detector.implementation_sha256"
    )
    if len(implementation_sha256) != SHA256_HEX_LENGTH or any(
        character not in "0123456789abcdef" for character in implementation_sha256
    ):
        message = "policy_lock.detector.implementation_sha256 must be lowercase SHA-256"
        raise ValueError(message)
    configs = _object(detector["config_by_device"], "policy_lock.detector.config_by_device")
    if not configs or any(not isinstance(config, dict) for config in configs.values()):
        message = "policy_lock.detector.config_by_device must contain configuration objects"
        raise ValueError(message)
    return PolicyLock(
        policy_id=_text(encoded["policy_id"], "policy_lock.policy_id"),
        frozen_at_utc=_timestamp(encoded["frozen_at_utc"], "policy_lock.frozen_at_utc"),
        development_recording_ids=development_ids,
        detector=detector,
        evaluation=_parse_evaluation(encoded["evaluation"]),
        canonical_sha256=hashlib.sha256(_canonical_json(encoded)).hexdigest(),
    )


def _parse_candidates(value: object, label: str, duration_ms: int) -> tuple[Candidate, ...]:
    candidates: list[Candidate] = []
    for index, raw in enumerate(_array(value, label)):
        item_label = f"{label}[{index}]"
        encoded = _object(raw, item_label)
        _exact_keys(encoded, {"strike_ms", "decision_ms"}, item_label)
        candidate = Candidate(
            strike_ms=_integer(encoded["strike_ms"], f"{item_label}.strike_ms"),
            decision_ms=_integer(encoded["decision_ms"], f"{item_label}.decision_ms"),
        )
        if candidate.decision_ms < candidate.strike_ms or candidate.decision_ms > duration_ms:
            message = f"{item_label} timing is invalid"
            raise ValueError(message)
        candidates.append(candidate)
    if any(
        current.decision_ms < previous.decision_ms
        for previous, current in itertools.pairwise(candidates)
    ):
        message = f"{label} must be ordered by online decision time"
        raise ValueError(message)
    return tuple(candidates)


def _attempts(session: manifest.Session, role: str) -> dict[str, tuple[int, int]]:
    attempts: dict[str, tuple[int, int]] = {}
    for episode in session.episodes:
        if episode.pose_expectation != "must_arm":
            continue
        view = episode.views[role]
        preferred = cast("int", view.preferred_arm_ms)
        attempts[episode.episode_id] = (preferred, view.end_ms)
        if episode.category == "clear_and_rearm":
            attempts.pop(episode.episode_id)
            attempts[f"{episode.episode_id}:initial"] = (preferred, view.end_ms)
            attempts[f"{episode.episode_id}:rearm"] = (
                cast("int", view.rearm_preferred_ms),
                view.end_ms,
            )
    return attempts


def armed_attempt_windows(
    session: manifest.Session, role: str, policy: PolicyLock
) -> tuple[tuple[str, int, int], ...]:
    """Return the exact reset-from-arm replay windows required by the gate."""
    duration = session.streams[role].duration_ms
    return tuple(
        (attempt_id, arm_ms, min(arm_ms + policy.evaluation.maximum_armed_ms, duration))
        for attempt_id, (arm_ms, _) in _attempts(session, role).items()
    )


def _parse_predictions(  # noqa: C901, PLR0912, PLR0915
    text: str, corpus: manifest.Corpus, policy: PolicyLock
) -> tuple[SessionPredictions, ...]:
    raw = _decode_json(text)
    encoded = _object(raw, "predictions")
    _exact_keys(
        encoded,
        {
            "schema_version",
            "report_type",
            "policy_id",
            "policy_lock_sha256",
            "detector",
            "sessions",
        },
        "predictions",
    )
    if encoded["schema_version"] != SCHEMA_VERSION or encoded["report_type"] != REPORT_TYPE:
        message = "unsupported audio holdout prediction report"
        raise ValueError(message)
    if encoded["policy_id"] != policy.policy_id:
        message = "prediction policy ID disagrees with the frozen policy"
        raise ValueError(message)
    if encoded["policy_lock_sha256"] != policy.canonical_sha256:
        message = "prediction policy-lock SHA-256 disagrees with the frozen policy"
        raise ValueError(message)
    if encoded["detector"] != policy.detector:
        message = "prediction detector configuration disagrees with the frozen policy"
        raise ValueError(message)
    sessions_by_id = {session.recording_id: session for session in corpus.sessions}
    parsed: list[SessionPredictions] = []
    seen: set[str] = set()
    for session_index, raw_session in enumerate(
        _array(encoded["sessions"], "predictions.sessions")
    ):
        label = f"predictions.sessions[{session_index}]"
        item = _object(raw_session, label)
        _exact_keys(item, {"recording_id", "streams"}, label)
        recording_id = _text(item["recording_id"], f"{label}.recording_id")
        if recording_id in seen or recording_id not in sessions_by_id:
            message = f"{label} has a duplicate or unknown recording ID"
            raise ValueError(message)
        seen.add(recording_id)
        session = sessions_by_id[recording_id]
        raw_streams = _object(item["streams"], f"{label}.streams")
        _exact_keys(raw_streams, set(manifest.ROLES), f"{label}.streams")
        streams: dict[str, StreamPredictions] = {}
        for role in manifest.ROLES:
            stream_label = f"{label}.streams.{role}"
            stream = _object(raw_streams[role], stream_label)
            _exact_keys(stream, {"continuous_candidates", "armed_replays"}, stream_label)
            duration = session.streams[role].duration_ms
            replays: dict[str, ArmedReplay] = {}
            for replay_index, raw_replay in enumerate(
                _array(stream["armed_replays"], f"{stream_label}.armed_replays")
            ):
                replay_label = f"{stream_label}.armed_replays[{replay_index}]"
                replay = _object(raw_replay, replay_label)
                _exact_keys(
                    replay,
                    {"attempt_id", "arm_ms", "evaluation_end_ms", "candidates"},
                    replay_label,
                )
                attempt_id = _text(replay["attempt_id"], f"{replay_label}.attempt_id")
                if attempt_id in replays:
                    message = f"{stream_label} has duplicate armed replay {attempt_id}"
                    raise ValueError(message)
                arm_ms = _integer(replay["arm_ms"], f"{replay_label}.arm_ms")
                end_ms = _integer(replay["evaluation_end_ms"], f"{replay_label}.evaluation_end_ms")
                expected_end = min(arm_ms + policy.evaluation.maximum_armed_ms, duration)
                if end_ms != expected_end:
                    message = f"{replay_label} does not cover the full locked armed window"
                    raise ValueError(message)
                replays[attempt_id] = ArmedReplay(
                    attempt_id=attempt_id,
                    arm_ms=arm_ms,
                    evaluation_end_ms=end_ms,
                    candidates=_parse_candidates(
                        replay["candidates"], f"{replay_label}.candidates", duration
                    ),
                )
                if any(
                    candidate.strike_ms < arm_ms or candidate.decision_ms > end_ms
                    for candidate in replays[attempt_id].candidates
                ):
                    message = f"{replay_label} contains a candidate outside its armed replay"
                    raise ValueError(message)
            expected_attempts = _attempts(session, role)
            if set(replays) != set(expected_attempts):
                message = f"{stream_label} armed replay coverage is incomplete"
                raise ValueError(message)
            for attempt_id, (arm_ms, _) in expected_attempts.items():
                if replays[attempt_id].arm_ms != arm_ms:
                    message = f"{stream_label}.{attempt_id} does not use the reviewed preferred arm"
                    raise ValueError(message)
            streams[role] = StreamPredictions(
                continuous_candidates=_parse_candidates(
                    stream["continuous_candidates"],
                    f"{stream_label}.continuous_candidates",
                    duration,
                ),
                armed_replays=replays,
            )
        parsed.append(SessionPredictions(recording_id=recording_id, streams=streams))
    return tuple(parsed)


def _rate_ppm(numerator: int, denominator: int) -> int:
    return 0 if denominator == 0 else numerator * 1_000_000 // denominator


def _one_to_one_target_matches(
    targets_ms: Sequence[int], candidates: Sequence[Candidate], tolerance_ms: int
) -> tuple[dict[int, int], set[int]]:
    """Maximally match target and candidate timestamps without reusing either one."""
    target_order = sorted(range(len(targets_ms)), key=lambda index: (targets_ms[index], index))
    candidate_order = sorted(
        range(len(candidates)),
        key=lambda index: (candidates[index].strike_ms, candidates[index].decision_ms, index),
    )
    matches: dict[int, int] = {}
    unmatched_candidates = set(candidate_order)
    target_cursor = 0
    candidate_cursor = 0
    while target_cursor < len(target_order) and candidate_cursor < len(candidate_order):
        target_index = target_order[target_cursor]
        candidate_index = candidate_order[candidate_cursor]
        target_ms = targets_ms[target_index]
        candidate_ms = candidates[candidate_index].strike_ms
        if candidate_ms < target_ms - tolerance_ms:
            candidate_cursor += 1
        elif candidate_ms > target_ms + tolerance_ms:
            target_cursor += 1
        else:
            matches[target_index] = candidate_index
            unmatched_candidates.remove(candidate_index)
            target_cursor += 1
            candidate_cursor += 1
    return matches, unmatched_candidates


def _candidate_report(
    sessions: Sequence[manifest.Session],
    predictions: Mapping[str, SessionPredictions],
    policy: PolicyLock,
) -> dict[str, object]:
    target_total = 0
    target_recalled = 0
    quiet_total = 0
    quiet_recalled = 0
    negative_counts: dict[str, int] = dict.fromkeys(
        sorted(manifest.REQUIRED_AUDIO_NEGATIVE_CATEGORIES), 0
    )
    non_target_candidate_count = 0
    streams: list[dict[str, object]] = []
    tolerance = policy.evaluation.target_tolerance_ms
    for session in sessions:
        prediction = predictions[session.recording_id]
        for role in manifest.ROLES:
            candidates = prediction.streams[role].continuous_candidates
            target_episodes = [
                episode
                for episode in session.episodes
                if episode.audio_expectation == "must_trigger"
            ]
            targets = [cast("int", episode.views[role].reference_ms) for episode in target_episodes]
            target_matches, unmatched_candidates = _one_to_one_target_matches(
                targets, candidates, tolerance
            )
            non_target_candidate_count += len(unmatched_candidates)
            target_index_by_episode = {
                episode.episode_id: index for index, episode in enumerate(target_episodes)
            }
            target_rows: list[dict[str, object]] = []
            negative_rows: list[dict[str, object]] = []
            for episode in session.episodes:
                view = episode.views[role]
                if episode.audio_expectation == "must_trigger":
                    matched = target_index_by_episode[episode.episode_id] in target_matches
                    target_total += 1
                    target_recalled += matched
                    is_quiet = episode.audio_character == "quiet"
                    quiet_total += is_quiet
                    quiet_recalled += is_quiet and matched
                    target_rows.append(
                        {
                            "episode_id": episode.episode_id,
                            "recalled": matched,
                            "match_count": int(matched),
                        }
                    )
                elif episode.audio_expectation == "must_not_trigger":
                    matched = [
                        candidate
                        for candidate in candidates
                        if view.start_ms <= candidate.strike_ms < view.end_ms
                    ]
                    if episode.category in negative_counts:
                        negative_counts[episode.category] += len(matched)
                    negative_rows.append(
                        {
                            "episode_id": episode.episode_id,
                            "category": episode.category,
                            "candidate_count": len(matched),
                        }
                    )
            streams.append(
                {
                    "recording_id": session.recording_id,
                    "role": role,
                    "continuous_candidate_count": len(candidates),
                    "targets": target_rows,
                    "negative_episodes": negative_rows,
                }
            )
    return {
        "target_count": target_total,
        "target_recalled": target_recalled,
        "target_recall_ppm": _rate_ppm(target_recalled, target_total),
        "quiet_target_count": quiet_total,
        "quiet_target_recalled": quiet_recalled,
        "quiet_target_recall_ppm": _rate_ppm(quiet_recalled, quiet_total),
        "non_target_candidate_count": non_target_candidate_count,
        "labeled_negative_candidate_count": sum(negative_counts.values()),
        "labeled_negative_candidates_by_category": negative_counts,
        "streams": streams,
    }


def _episode_for_attempt(session: manifest.Session, attempt_id: str) -> manifest.Episode:
    episode_id = attempt_id.split(":", maxsplit=1)[0]
    return next(episode for episode in session.episodes if episode.episode_id == episode_id)


def _lifecycle_report(  # noqa: C901, PLR0915
    sessions: Sequence[manifest.Session],
    predictions: Mapping[str, SessionPredictions],
    policy: PolicyLock,
) -> dict[str, object]:
    target_ids = {
        f"{session.recording_id}/{episode.episode_id}"
        for session in sessions
        for episode in session.episodes
        if episode.audio_expectation == "must_trigger"
    }
    captured: set[str] = set()
    false_terminals = 0
    high_speed_ms = 0
    reviewed_ms = sum(
        session.streams[policy.evaluation.leader_role].duration_ms for session in sessions
    )
    attempts_json: list[dict[str, object]] = []
    ignored_attempt_ids: list[str] = []
    required_attempt_count = 0
    evaluated_attempt_count = 0
    for session in sessions:
        role = policy.evaluation.leader_role
        stream = predictions[session.recording_id].streams[role]
        ordered = sorted(
            stream.armed_replays.values(), key=lambda replay: (replay.arm_ms, replay.attempt_id)
        )
        required_attempt_count += len(ordered)
        busy_until = 0
        for replay in ordered:
            episode = _episode_for_attempt(session, replay.attempt_id)
            if replay.arm_ms < busy_until:
                ignored_attempt_ids.append(f"{session.recording_id}/{replay.attempt_id}")
                attempts_json.append(
                    {
                        "recording_id": session.recording_id,
                        "attempt_id": replay.attempt_id,
                        "outcome": "ignored_while_busy",
                    }
                )
                continue
            evaluated_attempt_count += 1
            ready = replay.arm_ms + policy.evaluation.audio_ready_delay_ms
            clear_at = (
                cast("int", episode.views[role].clear_ms)
                if episode.category == "clear_and_rearm" and replay.attempt_id.endswith(":initial")
                else None
            )
            terminal = next(
                (
                    candidate
                    for candidate in replay.candidates
                    if candidate.decision_ms >= ready
                    and (clear_at is None or candidate.decision_ms < clear_at)
                ),
                None,
            )
            if terminal is None and clear_at is not None:
                terminal_at = clear_at
                completion = clear_at
                outcome = "pose_clear"
            elif terminal is None:
                terminal_at = min(
                    replay.arm_ms + policy.evaluation.maximum_armed_ms,
                    session.streams[role].duration_ms,
                )
                completion = min(
                    terminal_at + policy.evaluation.post_terminal_ms,
                    session.streams[role].duration_ms,
                )
                outcome = "no_impact_timeout"
            else:
                terminal_at = terminal.decision_ms
                completion = min(
                    terminal.decision_ms + policy.evaluation.post_terminal_ms,
                    session.streams[role].duration_ms,
                )
                if (
                    episode.audio_expectation == "must_trigger"
                    and abs(terminal.strike_ms - cast("int", episode.views[role].reference_ms))
                    <= policy.evaluation.target_tolerance_ms
                ):
                    outcome = "target_terminal"
                elif episode.audio_expectation == "diagnostic":
                    outcome = "diagnostic_terminal"
                else:
                    outcome = "false_terminal"
                    false_terminals += 1
            high_speed_ms += max(0, completion - replay.arm_ms)
            busy_until = completion + policy.evaluation.rearm_delay_ms
            retained_end = completion
            retained_start = max(
                replay.arm_ms + policy.evaluation.video_ready_delay_ms,
                (terminal.strike_ms if terminal is not None else terminal_at)
                - policy.evaluation.retained_history_ms,
            )
            for target in session.episodes:
                if target.audio_expectation != "must_trigger":
                    continue
                view = target.views[role]
                reference = cast("int", view.reference_ms)
                takeaway = cast("int", view.takeaway_ms)
                if (
                    retained_start <= reference <= retained_end
                    and replay.arm_ms + policy.evaluation.video_ready_delay_ms <= takeaway
                ):
                    captured.add(f"{session.recording_id}/{target.episode_id}")
            attempts_json.append(
                {
                    "recording_id": session.recording_id,
                    "attempt_id": replay.attempt_id,
                    "outcome": outcome,
                    "arm_ms": replay.arm_ms,
                    "ready_ms": ready,
                    "terminal_strike_ms": None if terminal is None else terminal.strike_ms,
                    "decision_ms": terminal_at,
                    "completion_ms": completion,
                }
            )
    return {
        "leader_role": policy.evaluation.leader_role,
        "target_count": len(target_ids),
        "captured_target_count": len(captured),
        "captured_target_ids": sorted(captured),
        "target_capture_ppm": _rate_ppm(len(captured), len(target_ids)),
        "false_terminal_attempt_count": false_terminals,
        "required_attempt_count": required_attempt_count,
        "evaluated_attempt_count": evaluated_attempt_count,
        "ignored_attempt_count": len(ignored_attempt_ids),
        "ignored_attempt_ids": ignored_attempt_ids,
        "high_speed_ms": high_speed_ms,
        "reviewed_ms": reviewed_ms,
        "high_speed_duty_ppm": _rate_ppm(high_speed_ms, reviewed_ms),
        "attempts": attempts_json,
    }


def evaluate_audio_holdout(
    corpus: manifest.Corpus,
    policy_lock_text: str,
    predictions_text: str,
    *,
    assets_verified: bool,
    source_capture_times_utc: Mapping[str, datetime.datetime],
) -> dict[str, object]:
    """Score all qualifying holdouts only after provenance and coverage gates pass."""
    readiness = manifest.readiness_report(corpus, assets_verified=assets_verified)
    if not readiness["audio_detector_selection_ready"]:
        message = "audio holdout evidence is incomplete: " + "; ".join(
            cast("list[str]", readiness["gaps"])
        )
        raise ValueError(message)
    missing_lifecycle = cast("list[str]", readiness["missing_pose_categories"])
    if missing_lifecycle:
        message = "audio holdout lifecycle evidence is incomplete: " + ", ".join(missing_lifecycle)
        raise ValueError(message)
    policy = parse_policy_lock(policy_lock_text)
    if policy.development_recording_ids != corpus.development_recording_ids:
        message = "policy lock selection IDs do not exactly match declared development evidence"
        raise ValueError(message)
    holdout_ids = set(cast("list[str]", readiness["phone_view_swapped_holdout_ids"]))
    if holdout_ids.intersection(policy.development_recording_ids):
        message = "policy lock leaks a holdout recording into detector selection"
        raise ValueError(message)
    if set(source_capture_times_utc) != holdout_ids:
        message = "source capture times do not cover exactly the qualifying holdouts"
        raise ValueError(message)
    if any(
        policy.frozen_at_utc >= source_capture_times_utc[recording_id]
        for recording_id in holdout_ids
    ):
        message = "detector policy was not frozen before every holdout capture"
        raise ValueError(message)
    sessions = tuple(session for session in corpus.sessions if session.recording_id in holdout_ids)
    devices = {
        session.streams[role].device_label for session in sessions for role in manifest.ROLES
    }
    configs = cast("Mapping[str, object]", policy.detector["config_by_device"])
    if set(configs) != devices:
        message = "frozen detector configurations do not cover exactly the holdout devices"
        raise ValueError(message)
    parsed_predictions = _parse_predictions(predictions_text, corpus, policy)
    if {prediction.recording_id for prediction in parsed_predictions} != holdout_ids:
        message = "predictions must cover every qualifying holdout exactly once"
        raise ValueError(message)
    predictions = {prediction.recording_id: prediction for prediction in parsed_predictions}
    candidate_generation = _candidate_report(sessions, predictions, policy)
    lifecycle = _lifecycle_report(sessions, predictions, policy)
    checks: dict[str, bool] = {
        "target_recall": cast("int", candidate_generation["target_recall_ppm"])
        >= policy.evaluation.minimum_target_recall_ppm,
        "quiet_target_recall": cast("int", candidate_generation["quiet_target_recall_ppm"])
        >= policy.evaluation.minimum_quiet_target_recall_ppm,
        "negative_continuous_candidates": cast(
            "int", candidate_generation["non_target_candidate_count"]
        )
        <= policy.evaluation.maximum_negative_continuous_candidates,
        "lifecycle_capture": cast("int", lifecycle["target_capture_ppm"])
        >= policy.evaluation.minimum_lifecycle_capture_ppm,
        "lifecycle_attempt_coverage": cast("int", lifecycle["ignored_attempt_count"]) == 0,
        "false_terminal_attempts": cast("int", lifecycle["false_terminal_attempt_count"])
        <= policy.evaluation.maximum_false_terminal_attempts,
        "high_speed_duty": cast("int", lifecycle["high_speed_duty_ppm"])
        <= policy.evaluation.maximum_high_speed_duty_ppm,
    }
    return {
        "schema_version": SCHEMA_VERSION,
        "report_type": "audio_detector_holdout_evaluation",
        "passed": all(checks.values()),
        "corpus_id": corpus.corpus_id,
        "holdout_evidence_sha256": _holdout_evidence_sha256(corpus, sessions),
        "holdout_recording_ids": sorted(holdout_ids),
        "policy_id": policy.policy_id,
        "policy_lock_sha256": policy.canonical_sha256,
        "detector": policy.detector,
        "evaluation_policy": {
            **dataclasses.asdict(policy.evaluation),
            "continuous_candidate_matching": CONTINUOUS_CANDIDATE_MATCHING,
        },
        "leakage_controls": {
            "development_recording_ids": list(policy.development_recording_ids),
            "policy_frozen_before_all_captures": True,
            "phone_view_swapped": True,
            "complete_timeline_review": True,
            "assets_hash_verified": assets_verified,
            "all_qualifying_holdouts_scored": True,
            "single_frozen_policy": True,
        },
        "checks": checks,
        "candidate_generation": candidate_generation,
        "lifecycle": lifecycle,
    }


def source_capture_times(corpus: manifest.Corpus, media_root: Path) -> dict[str, datetime.datetime]:
    """Read capture instants from the already hash-verified source manifests."""
    root = media_root.resolve(strict=True)
    readiness = manifest.readiness_report(corpus, assets_verified=True)
    selected = set(cast("list[str]", readiness["phone_view_swapped_holdout_ids"]))
    result: dict[str, datetime.datetime] = {}
    for session in corpus.sessions:
        if session.recording_id not in selected:
            continue
        captures: list[datetime.datetime] = []
        for role in manifest.ROLES:
            asset = session.streams[role].source_manifest
            source_path = root / Path(*asset.path.parts)
            source = _object(
                _decode_json(source_path.read_text(encoding="utf-8")), "source manifest"
            )
            captures.append(
                _timestamp(
                    source.get("created_at_utc"), f"{session.recording_id}/{role}.created_at_utc"
                )
            )
        result[session.recording_id] = min(captures)
    return result
