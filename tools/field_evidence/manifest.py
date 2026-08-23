"""Strict contract and readiness audit for paired field-evaluation recordings."""

from __future__ import annotations

import argparse
import dataclasses
import datetime
import hashlib
import json
import os
import re
import tempfile
from itertools import pairwise
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, cast, final

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence


SCHEMA_VERSION = 1
READINESS_POLICY_VERSION = "field_readiness_v2"
MINIMUM_PHONE_VIEW_SWAPPED_HOLDOUT_SESSIONS = 2
MINIMUM_EPISODES_PER_REQUIRED_CATEGORY = 2
MINIMUM_DISTINCT_SESSIONS_PER_REQUIRED_CATEGORY = 2
MINIMUM_QUIET_REAL_IMPACT_EPISODES = 2
MINIMUM_DISTINCT_QUIET_REAL_IMPACT_SESSIONS = 2
MINIMUM_RELATIVE_ASSET_PATH_PARTS = 2
MINIMUM_POSE_DIVERSITY_VALUES = 2
ROLES = ("down_the_line", "face_on")
SPLITS = frozenset({"development", "validation", "challenge"})
REVIEW_STATES = frozenset({"needs_review", "partial", "complete"})
POSE_EXPECTATIONS = frozenset({"must_arm", "must_not_arm", "diagnostic"})
AUDIO_EXPECTATIONS = frozenset({"must_trigger", "must_not_trigger", "diagnostic"})
AUDIO_CHARACTERS = frozenset({"quiet", "nominal", "loud", "not_applicable"})
CATEGORIES = frozenset(
    {
        "real_swing",
        "practice_swing",
        "mat_strike",
        "waggle",
        "speech",
        "footsteps",
        "club_drop",
        "aborted_address",
        "address_no_swing",
        "clear_and_rearm",
        "empty_scene",
        "walk_through",
        "post_shot_finish",
        "repeated_setup",
    }
)
REQUIRED_POSE_CATEGORIES = frozenset(
    {
        "real_swing",
        "practice_swing",
        "aborted_address",
        "address_no_swing",
        "clear_and_rearm",
        "empty_scene",
        "walk_through",
        "post_shot_finish",
        "repeated_setup",
    }
)
REQUIRED_AUDIO_NEGATIVE_CATEGORIES = frozenset(
    {
        "practice_swing",
        "mat_strike",
        "waggle",
        "speech",
        "footsteps",
        "club_drop",
        "aborted_address",
    }
)
ID_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}")
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
RFC3339_DATE_TIME_PATTERN = r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}"
RFC3339_ZONE_PATTERN = r"(?:\.[0-9]{1,6})?(?:Z|[+-][0-9]{2}:[0-9]{2})"
RFC3339_PATTERN = re.compile(RFC3339_DATE_TIME_PATTERN + RFC3339_ZONE_PATTERN)
POSE_CATEGORY_EXPECTATIONS: Mapping[str, str] = {
    "real_swing": "must_arm",
    "practice_swing": "must_not_arm",
    "aborted_address": "must_arm",
    "address_no_swing": "must_arm",
    "clear_and_rearm": "must_arm",
    "empty_scene": "must_not_arm",
    "walk_through": "must_not_arm",
    "post_shot_finish": "must_not_arm",
    "repeated_setup": "must_not_arm",
}
AUDIO_CATEGORY_EXPECTATIONS: Mapping[str, str] = {
    "real_swing": "must_trigger",
    "practice_swing": "must_not_trigger",
    "mat_strike": "must_not_trigger",
    "waggle": "must_not_trigger",
    "speech": "must_not_trigger",
    "footsteps": "must_not_trigger",
    "club_drop": "must_not_trigger",
    "aborted_address": "must_not_trigger",
    "address_no_swing": "must_not_trigger",
}

COLLECTION_SCENARIO_ORDER = (
    "empty_scene",
    "walk_through",
    "speech",
    "footsteps",
    "club_drop",
    "mat_strike",
    "practice_swing",
    "aborted_address",
    "address_no_swing",
    "repeated_setup",
    "clear_and_rearm",
    "waggle",
    "real_swing",
    "post_shot_finish",
)
COLLECTION_SCENARIO_INSTRUCTIONS: Mapping[str, str] = {
    "empty_scene": "Leave both camera views empty for at least five seconds.",
    "walk_through": "Walk through the hitting area without setting up to the ball.",
    "speech": "Speak at normal and emphatic volume without swinging or striking anything.",
    "footsteps": "Walk and shuffle near both phones without assuming golf address.",
    "club_drop": "Drop or set down a club in the ordinary hitting area without ball contact.",
    "mat_strike": "Strike the mat without contacting a ball.",
    "practice_swing": "Make a realistic full-speed practice swing with no ball contact.",
    "aborted_address": "Reach a valid address, pause, then leave without beginning a swing.",
    "address_no_swing": "Hold a valid address for several seconds, then reset without swinging.",
    "repeated_setup": (
        "Make several partial or unsquared setup attempts without reaching a stable valid address."
    ),
    "clear_and_rearm": (
        "Reach valid address, leave the scene clearly, return to valid address, then hit one ball."
    ),
    "waggle": "At valid address, make realistic waggles without impact before resetting.",
    "real_swing": "Hit a real ball; include the quietest representative valid impact in this set.",
    "post_shot_finish": (
        "After a completed shot, hold the finish and move normally without starting a new setup."
    ),
}


@dataclasses.dataclass(frozen=True)
class Asset:
    """One root-relative, optionally hash-pinned recording asset."""

    path: PurePosixPath
    sha256: str | None


@dataclasses.dataclass(frozen=True)
class Interval:
    """One nonempty half-open interval on a stream-local media timeline."""

    start_ms: int
    end_ms: int
    note: str


@dataclasses.dataclass(frozen=True)
class ViewLabels:
    """Human labels expressed on one role's local audio/video timeline."""

    start_ms: int
    end_ms: int
    reference_ms: int | None
    safe_arm_start_ms: int | None
    preferred_arm_ms: int | None
    takeaway_ms: int | None
    clear_ms: int | None
    rearm_safe_start_ms: int | None
    rearm_preferred_ms: int | None


@dataclasses.dataclass(frozen=True)
class Episode:
    """One human-classified field episode with labels for both recorded views."""

    episode_id: str
    category: str
    pose_expectation: str
    audio_expectation: str
    audio_character: str
    views: Mapping[str, ViewLabels]
    notes: str


@dataclasses.dataclass(frozen=True)
class Stream:
    """Evidence and review coverage for one phone/view stream."""

    node_id: str
    device_label: str
    duration_ms: int
    source_manifest: Asset
    video: Asset
    audio: Asset
    reviewed_intervals: tuple[Interval, ...]


@dataclasses.dataclass(frozen=True)
class Review:
    """Explicit human-review provenance; never inferred from populated labels."""

    state: str
    reviewer: str | None
    reviewed_at_utc: str | None


@dataclasses.dataclass(frozen=True)
class Scene:
    """Human-readable diversity strata, kept independent of device/view assignment."""

    golfer_label: str
    environment_label: str
    lighting_label: str
    framing_label: str


@dataclasses.dataclass(frozen=True)
class Session:
    """One coordinated two-phone field recording."""

    recording_id: str
    split: str
    scene: Scene
    streams: Mapping[str, Stream]
    review: Review
    episodes: tuple[Episode, ...]
    notes: str


@dataclasses.dataclass(frozen=True)
class Corpus:
    """Development provenance plus independently reviewed holdout sessions."""

    corpus_id: str
    development_recording_ids: tuple[str, ...]
    development_device_assignment: Mapping[str, str]
    sessions: tuple[Session, ...]


def _object(value: object, label: str) -> dict[str, object]:
    if not isinstance(value, dict):
        message = f"{label} must be an object with string keys"
        raise TypeError(message)
    mapping = cast("dict[object, object]", value)
    if not all(isinstance(key, str) for key in mapping):
        message = f"{label} must be an object with string keys"
        raise TypeError(message)
    return cast("dict[str, object]", mapping)


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


def _text(value: object, label: str) -> str:
    if not isinstance(value, str) or not value.strip():
        message = f"{label} must be nonempty text"
        raise TypeError(message)
    return value


def _optional_text(value: object, label: str) -> str | None:
    return None if value is None else _text(value, label)


def _review_timestamp(value: object, label: str) -> str:
    encoded = _text(value, label)
    if RFC3339_PATTERN.fullmatch(encoded) is None:
        message = f"{label} must be a timezone-aware RFC 3339 timestamp"
        raise ValueError(message)
    try:
        parsed = datetime.datetime.fromisoformat(encoded.replace("Z", "+00:00"))
    except ValueError as error:
        message = f"{label} must be a valid RFC 3339 timestamp"
        raise ValueError(message) from error
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        message = f"{label} must include a timezone"
        raise ValueError(message)
    utc = parsed.astimezone(datetime.UTC)
    timespec = "seconds" if utc.microsecond == 0 else "microseconds"
    return utc.isoformat(timespec=timespec).replace("+00:00", "Z")


def _identifier(value: object, label: str) -> str:
    parsed = _text(value, label)
    if ID_PATTERN.fullmatch(parsed) is None:
        message = f"{label} is not a bounded identifier"
        raise ValueError(message)
    return parsed


def _integer(value: object, label: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        message = f"{label} must be an integer"
        raise TypeError(message)
    return value


def _optional_integer(value: object, label: str) -> int | None:
    return None if value is None else _integer(value, label)


def _enum(value: object, allowed: frozenset[str], label: str) -> str:
    parsed = _text(value, label)
    if parsed not in allowed:
        message = f"{label} has unsupported value {parsed!r}"
        raise ValueError(message)
    return parsed


def _parse_asset(value: object, label: str) -> Asset:
    encoded = _object(value, label)
    _exact_keys(encoded, {"path", "sha256"}, label)
    path = PurePosixPath(_text(encoded["path"], f"{label}.path"))
    if (
        path.is_absolute()
        or ".." in path.parts
        or len(path.parts) < MINIMUM_RELATIVE_ASSET_PATH_PARTS
    ):
        message = f"{label}.path must be a safe relative path beneath the corpus root"
        raise ValueError(message)
    raw_digest = encoded["sha256"]
    digest = None if raw_digest is None else _text(raw_digest, f"{label}.sha256")
    if digest is not None and SHA256_PATTERN.fullmatch(digest) is None:
        message = f"{label}.sha256 must be lowercase hexadecimal SHA-256"
        raise ValueError(message)
    return Asset(path=path, sha256=digest)


def _parse_interval(value: object, label: str, duration_ms: int) -> Interval:
    encoded = _object(value, label)
    _exact_keys(encoded, {"start_ms", "end_ms", "note"}, label)
    interval = Interval(
        start_ms=_integer(encoded["start_ms"], f"{label}.start_ms"),
        end_ms=_integer(encoded["end_ms"], f"{label}.end_ms"),
        note=_text(encoded["note"], f"{label}.note"),
    )
    if interval.start_ms < 0 or interval.end_ms <= interval.start_ms:
        message = f"{label} must be a nonempty nonnegative half-open interval"
        raise ValueError(message)
    if interval.end_ms > duration_ms:
        message = f"{label} exceeds its stream duration"
        raise ValueError(message)
    return interval


def _parse_view_labels(value: object, label: str, duration_ms: int) -> ViewLabels:
    encoded = _object(value, label)
    expected = {
        "start_ms",
        "end_ms",
        "reference_ms",
        "safe_arm_start_ms",
        "preferred_arm_ms",
        "takeaway_ms",
        "clear_ms",
        "rearm_safe_start_ms",
        "rearm_preferred_ms",
    }
    _exact_keys(encoded, expected, label)
    view = ViewLabels(
        start_ms=_integer(encoded["start_ms"], f"{label}.start_ms"),
        end_ms=_integer(encoded["end_ms"], f"{label}.end_ms"),
        reference_ms=_optional_integer(encoded["reference_ms"], f"{label}.reference_ms"),
        safe_arm_start_ms=_optional_integer(
            encoded["safe_arm_start_ms"], f"{label}.safe_arm_start_ms"
        ),
        preferred_arm_ms=_optional_integer(
            encoded["preferred_arm_ms"], f"{label}.preferred_arm_ms"
        ),
        takeaway_ms=_optional_integer(encoded["takeaway_ms"], f"{label}.takeaway_ms"),
        clear_ms=_optional_integer(encoded["clear_ms"], f"{label}.clear_ms"),
        rearm_safe_start_ms=_optional_integer(
            encoded["rearm_safe_start_ms"], f"{label}.rearm_safe_start_ms"
        ),
        rearm_preferred_ms=_optional_integer(
            encoded["rearm_preferred_ms"], f"{label}.rearm_preferred_ms"
        ),
    )
    if view.start_ms < 0 or view.end_ms <= view.start_ms or view.end_ms > duration_ms:
        message = f"{label} interval is outside its stream duration"
        raise ValueError(message)
    for field in dataclasses.fields(ViewLabels):
        if field.name in {"start_ms", "end_ms"}:
            continue
        timestamp = cast("int | None", getattr(view, field.name))
        if timestamp is not None and not view.start_ms <= timestamp < view.end_ms:
            message = f"{label}.{field.name} must fall within the episode interval"
            raise ValueError(message)
    return view


def _parse_stream(value: object, role: str) -> Stream:
    label = f"streams.{role}"
    encoded = _object(value, label)
    _exact_keys(
        encoded,
        {
            "node_id",
            "device_label",
            "duration_ms",
            "source_manifest",
            "video",
            "audio",
            "reviewed_intervals",
        },
        label,
    )
    duration_ms = _integer(encoded["duration_ms"], f"{label}.duration_ms")
    if duration_ms <= 0:
        message = f"{label}.duration_ms must be positive"
        raise ValueError(message)
    intervals = tuple(
        _parse_interval(item, f"{label}.reviewed_intervals[{index}]", duration_ms)
        for index, item in enumerate(_array(encoded["reviewed_intervals"], label))
    )
    for previous, current in pairwise(intervals):
        if current.start_ms < previous.end_ms:
            message = f"{label}.reviewed_intervals must be sorted and nonoverlapping"
            raise ValueError(message)
    stream = Stream(
        node_id=_identifier(encoded["node_id"], f"{label}.node_id"),
        device_label=_identifier(encoded["device_label"], f"{label}.device_label"),
        duration_ms=duration_ms,
        source_manifest=_parse_asset(encoded["source_manifest"], f"{label}.source_manifest"),
        video=_parse_asset(encoded["video"], f"{label}.video"),
        audio=_parse_asset(encoded["audio"], f"{label}.audio"),
        reviewed_intervals=intervals,
    )
    assets = (stream.source_manifest.path, stream.video.path, stream.audio.path)
    if len(set(assets)) != len(assets):
        message = f"{label} source manifest, video, and audio paths must be distinct"
        raise ValueError(message)
    return stream


def _parse_review(value: object, label: str) -> Review:
    encoded = _object(value, label)
    _exact_keys(encoded, {"state", "reviewer", "reviewed_at_utc"}, label)
    review = Review(
        state=_enum(encoded["state"], REVIEW_STATES, f"{label}.state"),
        reviewer=_optional_text(encoded["reviewer"], f"{label}.reviewer"),
        reviewed_at_utc=(
            None
            if encoded["reviewed_at_utc"] is None
            else _review_timestamp(encoded["reviewed_at_utc"], f"{label}.reviewed_at_utc")
        ),
    )
    has_provenance = review.reviewer is not None and review.reviewed_at_utc is not None
    if review.state == "needs_review" and (
        review.reviewer is not None or review.reviewed_at_utc is not None
    ):
        message = f"{label} needs_review cannot claim review provenance"
        raise ValueError(message)
    if review.state != "needs_review" and not has_provenance:
        message = f"{label} partial/complete review requires reviewer and reviewed_at_utc"
        raise ValueError(message)
    return review


def _parse_scene(value: object, label: str) -> Scene:
    encoded = _object(value, label)
    _exact_keys(
        encoded,
        {"golfer_label", "environment_label", "lighting_label", "framing_label"},
        label,
    )
    return Scene(
        golfer_label=_identifier(encoded["golfer_label"], f"{label}.golfer_label"),
        environment_label=_identifier(encoded["environment_label"], f"{label}.environment_label"),
        lighting_label=_identifier(encoded["lighting_label"], f"{label}.lighting_label"),
        framing_label=_identifier(encoded["framing_label"], f"{label}.framing_label"),
    )


def _parse_episode(value: object, label: str, streams: Mapping[str, Stream]) -> Episode:
    encoded = _object(value, label)
    _exact_keys(
        encoded,
        {
            "id",
            "category",
            "pose_expectation",
            "audio_expectation",
            "audio_character",
            "views",
            "notes",
        },
        label,
    )
    raw_views = _object(encoded["views"], f"{label}.views")
    _exact_keys(raw_views, set(ROLES), f"{label}.views")
    views = {
        role: _parse_view_labels(
            raw_views[role], f"{label}.views.{role}", streams[role].duration_ms
        )
        for role in ROLES
    }
    episode = Episode(
        episode_id=_identifier(encoded["id"], f"{label}.id"),
        category=_enum(encoded["category"], CATEGORIES, f"{label}.category"),
        pose_expectation=_enum(
            encoded["pose_expectation"], POSE_EXPECTATIONS, f"{label}.pose_expectation"
        ),
        audio_expectation=_enum(
            encoded["audio_expectation"], AUDIO_EXPECTATIONS, f"{label}.audio_expectation"
        ),
        audio_character=_enum(
            encoded["audio_character"], AUDIO_CHARACTERS, f"{label}.audio_character"
        ),
        views=views,
        notes=_text(encoded["notes"], f"{label}.notes"),
    )
    _validate_episode_semantics(episode, label)
    return episode


def _validate_episode_semantics(episode: Episode, label: str) -> None:  # noqa: C901, PLR0912
    """Validate the intentionally centralized cross-field episode invariants."""
    expected_pose = POSE_CATEGORY_EXPECTATIONS.get(episode.category)
    if expected_pose is not None and episode.pose_expectation != expected_pose:
        message = f"{label} {episode.category} pose expectation must be {expected_pose}"
        raise ValueError(message)
    expected_audio = AUDIO_CATEGORY_EXPECTATIONS.get(episode.category)
    if expected_audio is not None and episode.audio_expectation != expected_audio:
        message = f"{label} {episode.category} audio expectation must be {expected_audio}"
        raise ValueError(message)
    for role, view in episode.views.items():
        arm_times = (view.safe_arm_start_ms, view.preferred_arm_ms)
        if episode.pose_expectation == "must_arm":
            if any(timestamp is None for timestamp in arm_times):
                message = f"{label}.{role} must_arm requires safe and preferred labels"
                raise ValueError(message)
            safe, preferred = cast("tuple[int, int]", arm_times)
            if safe > preferred:
                message = f"{label}.{role} must satisfy safe <= preferred"
                raise ValueError(message)
            if view.takeaway_ms is not None and preferred >= view.takeaway_ms:
                message = f"{label}.{role} preferred arm must precede takeaway"
                raise ValueError(message)
        elif any(timestamp is not None for timestamp in (*arm_times, view.takeaway_ms)):
            message = f"{label}.{role} non-must-arm episodes cannot define arm timing"
            raise ValueError(message)
        if episode.audio_expectation == "must_trigger" and view.reference_ms is None:
            message = f"{label}.{role} must_trigger requires a reference timestamp"
            raise ValueError(message)
        if episode.category == "real_swing":
            if (
                episode.pose_expectation != "must_arm"
                or episode.audio_expectation != "must_trigger"
            ):
                message = f"{label} real_swing must arm and must trigger"
                raise ValueError(message)
            if view.reference_ms is None or cast("int", view.takeaway_ms) >= view.reference_ms:
                message = f"{label}.{role} real_swing takeaway must precede impact reference"
                raise ValueError(message)
        rearm_times = (view.rearm_safe_start_ms, view.rearm_preferred_ms)
        if episode.category == "clear_and_rearm":
            if (
                view.clear_ms is None
                or view.takeaway_ms is None
                or any(timestamp is None for timestamp in rearm_times)
            ):
                message = (
                    f"{label}.{role} clear_and_rearm requires clear, rearm, and takeaway labels"
                )
                raise ValueError(message)
            safe = cast("int", view.safe_arm_start_ms)
            preferred = cast("int", view.preferred_arm_ms)
            rearm_safe, rearm_preferred = cast("tuple[int, int]", rearm_times)
            if not (
                safe <= preferred < view.clear_ms < rearm_safe <= rearm_preferred < view.takeaway_ms
            ):
                message = f"{label}.{role} clear_and_rearm lifecycle is out of order"
                raise ValueError(message)
        elif view.clear_ms is not None or any(timestamp is not None for timestamp in rearm_times):
            message = f"{label}.{role} non-rearm episode cannot define clear or rearm timing"
            raise ValueError(message)


def _parse_session(value: object, index: int) -> Session:
    label = f"sessions[{index}]"
    encoded = _object(value, label)
    _exact_keys(
        encoded,
        {"recording_id", "split", "scene", "streams", "review", "episodes", "notes"},
        label,
    )
    raw_streams = _object(encoded["streams"], f"{label}.streams")
    _exact_keys(raw_streams, set(ROLES), f"{label}.streams")
    streams = {role: _parse_stream(raw_streams[role], role) for role in ROLES}
    if streams[ROLES[0]].node_id == streams[ROLES[1]].node_id:
        message = f"{label} roles must come from distinct nodes"
        raise ValueError(message)
    if streams[ROLES[0]].device_label == streams[ROLES[1]].device_label:
        message = f"{label} roles must come from distinct device labels"
        raise ValueError(message)
    episodes = tuple(
        _parse_episode(item, f"{label}.episodes[{episode_index}]", streams)
        for episode_index, item in enumerate(_array(encoded["episodes"], f"{label}.episodes"))
    )
    episode_ids = [episode.episode_id for episode in episodes]
    if len(episode_ids) != len(set(episode_ids)):
        message = f"{label} episode ids must be unique"
        raise ValueError(message)
    for role in ROLES:
        role_views = [episode.views[role] for episode in episodes]
        for previous, current in pairwise(role_views):
            if current.start_ms < previous.end_ms:
                message = (
                    f"{label}.episodes must be chronological and nonoverlapping "
                    f"on the {role} timeline"
                )
                raise ValueError(message)
    return Session(
        recording_id=_identifier(encoded["recording_id"], f"{label}.recording_id"),
        split=_enum(encoded["split"], SPLITS, f"{label}.split"),
        scene=_parse_scene(encoded["scene"], f"{label}.scene"),
        streams=streams,
        review=_parse_review(encoded["review"], f"{label}.review"),
        episodes=episodes,
        notes=_text(encoded["notes"], f"{label}.notes"),
    )


def parse_manifest(text: str) -> Corpus:  # noqa: C901
    """Parse a complete evidence corpus without treating unreviewed rows as truth."""
    decoded = _decode_json(text)
    root = _object(decoded, "manifest")
    _exact_keys(
        root,
        {
            "schema_version",
            "corpus_id",
            "development_recording_ids",
            "development_device_assignment",
            "sessions",
        },
        "manifest",
    )
    if _integer(root["schema_version"], "schema_version") != SCHEMA_VERSION:
        message = "unsupported field-evidence schema_version"
        raise ValueError(message)
    raw_development_ids = _array(root["development_recording_ids"], "development_recording_ids")
    development_ids = tuple(
        _identifier(value, f"development_recording_ids[{index}]")
        for index, value in enumerate(raw_development_ids)
    )
    if not development_ids:
        message = "development_recording_ids must name the evidence used for prior tuning"
        raise ValueError(message)
    if len(development_ids) != len(set(development_ids)):
        message = "development_recording_ids must be unique"
        raise ValueError(message)
    raw_assignment = _object(root["development_device_assignment"], "development assignment")
    _exact_keys(raw_assignment, set(ROLES), "development_device_assignment")
    assignment = {
        role: _identifier(raw_assignment[role], f"development_device_assignment.{role}")
        for role in ROLES
    }
    if assignment[ROLES[0]] == assignment[ROLES[1]]:
        message = "development device assignment must name two distinct phones"
        raise ValueError(message)
    sessions = tuple(
        _parse_session(value, index)
        for index, value in enumerate(_array(root["sessions"], "sessions"))
    )
    if not sessions:
        message = "sessions must be nonempty"
        raise TypeError(message)
    recording_ids = [session.recording_id for session in sessions]
    if len(recording_ids) != len(set(recording_ids)):
        message = "session recording ids must be unique"
        raise ValueError(message)
    asset_paths = [
        asset.path
        for session in sessions
        for role in ROLES
        for asset in (
            session.streams[role].source_manifest,
            session.streams[role].video,
            session.streams[role].audio,
        )
    ]
    if len(asset_paths) != len(set(asset_paths)):
        message = "recording assets cannot be reused across roles or sessions"
        raise ValueError(message)
    node_by_device: dict[str, str] = {}
    device_by_node: dict[str, str] = {}
    for session in sessions:
        if session.split != "development" and session.recording_id in development_ids:
            message = f"holdout {session.recording_id!r} leaks a development recording"
            raise ValueError(message)
        for role in ROLES:
            stream = session.streams[role]
            prior_node = node_by_device.setdefault(stream.device_label, stream.node_id)
            prior_device = device_by_node.setdefault(stream.node_id, stream.device_label)
            if prior_node != stream.node_id or prior_device != stream.device_label:
                message = (
                    "device labels and durable node IDs must keep one stable one-to-one "
                    "binding across sessions"
                )
                raise ValueError(message)
    return Corpus(
        corpus_id=_identifier(root["corpus_id"], "corpus_id"),
        development_recording_ids=development_ids,
        development_device_assignment=assignment,
        sessions=sessions,
    )


def _full_review_coverage(stream: Stream) -> bool:
    cursor = 0
    for interval in stream.reviewed_intervals:
        if interval.start_ms != cursor:
            return False
        cursor = interval.end_ms
    return cursor == stream.duration_ms


def _assets_pinned(stream: Stream) -> bool:
    return all(
        asset.sha256 is not None for asset in (stream.source_manifest, stream.video, stream.audio)
    )


def _phone_view_swapped(corpus: Corpus, session: Session) -> bool:
    return all(
        session.streams[role].device_label == corpus.development_device_assignment[ROLES[1 - index]]
        for index, role in enumerate(ROLES)
    )


def _readiness_policy() -> dict[str, object]:
    return {
        "version": READINESS_POLICY_VERSION,
        "minimum_phone_view_swapped_holdout_sessions": (
            MINIMUM_PHONE_VIEW_SWAPPED_HOLDOUT_SESSIONS
        ),
        "minimum_episodes_per_required_category": MINIMUM_EPISODES_PER_REQUIRED_CATEGORY,
        "minimum_distinct_sessions_per_required_category": (
            MINIMUM_DISTINCT_SESSIONS_PER_REQUIRED_CATEGORY
        ),
        "minimum_quiet_real_impact_episodes": MINIMUM_QUIET_REAL_IMPACT_EPISODES,
        "minimum_distinct_quiet_real_impact_sessions": (
            MINIMUM_DISTINCT_QUIET_REAL_IMPACT_SESSIONS
        ),
    }


def _category_coverage(
    sessions: Sequence[Session],
    required_categories: frozenset[str],
    *,
    expectation: str,
) -> tuple[dict[str, int], dict[str, int]]:
    episode_counts = dict.fromkeys(sorted(required_categories), 0)
    session_counts = dict.fromkeys(sorted(required_categories), 0)
    for session in sessions:
        observed_in_session: set[str] = set()
        for episode in session.episodes:
            if episode.category not in required_categories:
                continue
            actual_expectation = (
                episode.pose_expectation if expectation == "pose" else episode.audio_expectation
            )
            if actual_expectation == "diagnostic":
                continue
            episode_counts[episode.category] += 1
            observed_in_session.add(episode.category)
        for category in observed_in_session:
            session_counts[category] += 1
    return episode_counts, session_counts


def _coverage_deficits(
    required_categories: frozenset[str],
    episode_counts: Mapping[str, int],
    session_counts: Mapping[str, int],
) -> tuple[dict[str, int], dict[str, int]]:
    episode_deficits = {
        category: max(
            0,
            MINIMUM_EPISODES_PER_REQUIRED_CATEGORY - episode_counts[category],
        )
        for category in sorted(required_categories)
    }
    session_deficits = {
        category: max(
            0,
            MINIMUM_DISTINCT_SESSIONS_PER_REQUIRED_CATEGORY - session_counts[category],
        )
        for category in sorted(required_categories)
    }
    return episode_deficits, session_deficits


def _collection_plan(  # noqa: PLR0913
    development_assignment: Mapping[str, str],
    *,
    missing_pose: Sequence[str],
    missing_audio: Sequence[str],
    pose_episode_deficits: Mapping[str, int],
    pose_session_deficits: Mapping[str, int],
    audio_episode_deficits: Mapping[str, int],
    audio_session_deficits: Mapping[str, int],
    quiet_real_impact_episode_deficit: int,
    quiet_real_impact_session_deficit: int,
    swapped_session_deficit: int,
    diversity: Mapping[str, Sequence[str]],
) -> dict[str, object]:
    needed_categories = {
        category
        for category in REQUIRED_POSE_CATEGORIES
        if pose_episode_deficits[category] or pose_session_deficits[category]
    } | {
        category
        for category in REQUIRED_AUDIO_NEGATIVE_CATEGORIES
        if audio_episode_deficits[category] or audio_session_deficits[category]
    }
    if quiet_real_impact_episode_deficit or quiet_real_impact_session_deficit:
        needed_categories.add("real_swing")
    scenario_sequence = [
        {
            "scenario_id": f"F{index:02d}",
            "category": category,
            "collect_for": sorted(
                purpose
                for purpose, categories in (
                    ("pose", REQUIRED_POSE_CATEGORIES),
                    ("audio", REQUIRED_AUDIO_NEGATIVE_CATEGORIES),
                )
                if category in categories
            )
            + (["audio_quiet_impact"] if category == "real_swing" else []),
            "additional_reviewed_occurrences_required": max(
                pose_episode_deficits.get(category, 0),
                audio_episode_deficits.get(category, 0),
                quiet_real_impact_episode_deficit if category == "real_swing" else 0,
            ),
            "additional_distinct_sessions_required": max(
                pose_session_deficits.get(category, 0),
                audio_session_deficits.get(category, 0),
                quiet_real_impact_session_deficit if category == "real_swing" else 0,
            ),
            "instruction": COLLECTION_SCENARIO_INSTRUCTIONS[category],
        }
        for index, category in enumerate(COLLECTION_SCENARIO_ORDER, start=1)
        if category in needed_categories
    ]
    return {
        "readiness_policy": _readiness_policy(),
        "required_phone_view_assignment": {
            ROLES[0]: development_assignment[ROLES[1]],
            ROLES[1]: development_assignment[ROLES[0]],
        },
        "additional_phone_view_swapped_holdout_sessions_required": swapped_session_deficit,
        "allowed_splits": sorted(SPLITS - {"development"}),
        "required_pose_categories": list(missing_pose),
        "required_audio_negative_categories": list(missing_audio),
        "pose_additional_occurrences_required": dict(pose_episode_deficits),
        "pose_additional_distinct_sessions_required": dict(pose_session_deficits),
        "audio_negative_additional_occurrences_required": dict(audio_episode_deficits),
        "audio_negative_additional_distinct_sessions_required": dict(audio_session_deficits),
        "quiet_real_impact_required": bool(
            quiet_real_impact_episode_deficit or quiet_real_impact_session_deficit
        ),
        "quiet_real_impact_additional_occurrences_required": (quiet_real_impact_episode_deficit),
        "quiet_real_impact_additional_distinct_sessions_required": (
            quiet_real_impact_session_deficit
        ),
        "additional_distinct_scene_labels_required": {
            label: max(0, 2 - len(diversity[label])) for label in ("golfer", "lighting", "framing")
        },
        "complete_timeline_review_required": True,
        "hash_pinned_media_required": True,
        "operator_protocol": [
            "Record both phones continuously with the required role assignment.",
            (
                "Before each scenario, show or speak its scenario_id, then leave at least two "
                "seconds of neutral separation before performing it."
            ),
            (
                "Keep every take, including unexpected noises and failed attempts; label them "
                "during complete timeline review rather than deleting them."
            ),
            "Do not use detector candidates as human ground truth.",
        ],
        "scenario_sequence": scenario_sequence,
    }


def initial_collection_plan(development_assignment: Mapping[str, str]) -> dict[str, object]:
    """Build the complete acquisition checklist before the first holdout is recorded."""
    if set(development_assignment) != set(ROLES):
        message = "development assignment must contain exactly both camera roles"
        raise ValueError(message)
    parsed_assignment = {
        role: _identifier(development_assignment[role], f"development_assignment.{role}")
        for role in ROLES
    }
    if parsed_assignment[ROLES[0]] == parsed_assignment[ROLES[1]]:
        message = "development assignment must name two distinct phones"
        raise ValueError(message)
    return _collection_plan(
        parsed_assignment,
        missing_pose=sorted(REQUIRED_POSE_CATEGORIES),
        missing_audio=sorted(REQUIRED_AUDIO_NEGATIVE_CATEGORIES),
        pose_episode_deficits=dict.fromkeys(
            sorted(REQUIRED_POSE_CATEGORIES), MINIMUM_EPISODES_PER_REQUIRED_CATEGORY
        ),
        pose_session_deficits=dict.fromkeys(
            sorted(REQUIRED_POSE_CATEGORIES),
            MINIMUM_DISTINCT_SESSIONS_PER_REQUIRED_CATEGORY,
        ),
        audio_episode_deficits=dict.fromkeys(
            sorted(REQUIRED_AUDIO_NEGATIVE_CATEGORIES), MINIMUM_EPISODES_PER_REQUIRED_CATEGORY
        ),
        audio_session_deficits=dict.fromkeys(
            sorted(REQUIRED_AUDIO_NEGATIVE_CATEGORIES),
            MINIMUM_DISTINCT_SESSIONS_PER_REQUIRED_CATEGORY,
        ),
        quiet_real_impact_episode_deficit=MINIMUM_QUIET_REAL_IMPACT_EPISODES,
        quiet_real_impact_session_deficit=(MINIMUM_DISTINCT_QUIET_REAL_IMPACT_SESSIONS),
        swapped_session_deficit=MINIMUM_PHONE_VIEW_SWAPPED_HOLDOUT_SESSIONS,
        diversity={"golfer": (), "lighting": (), "framing": ()},
    )


def readiness_report(  # noqa: C901, PLR0912, PLR0915
    corpus: Corpus, *, assets_verified: bool = False
) -> dict[str, object]:
    """Audit release-gate prerequisites without converting missing evidence into a pass."""
    holdouts = tuple(session for session in corpus.sessions if session.split != "development")
    reviewed_holdouts = tuple(
        session
        for session in holdouts
        if session.review.state == "complete"
        and all(_full_review_coverage(session.streams[role]) for role in ROLES)
        and all(_assets_pinned(session.streams[role]) for role in ROLES)
    )
    swapped_holdouts = tuple(
        session for session in reviewed_holdouts if _phone_view_swapped(corpus, session)
    )
    pose_episode_counts, pose_session_counts = _category_coverage(
        swapped_holdouts, REQUIRED_POSE_CATEGORIES, expectation="pose"
    )
    audio_episode_counts, audio_session_counts = _category_coverage(
        swapped_holdouts, REQUIRED_AUDIO_NEGATIVE_CATEGORIES, expectation="audio"
    )
    pose_episode_deficits, pose_session_deficits = _coverage_deficits(
        REQUIRED_POSE_CATEGORIES, pose_episode_counts, pose_session_counts
    )
    audio_episode_deficits, audio_session_deficits = _coverage_deficits(
        REQUIRED_AUDIO_NEGATIVE_CATEGORIES,
        audio_episode_counts,
        audio_session_counts,
    )
    categories = {episode.category for session in swapped_holdouts for episode in session.episodes}
    quiet_real_impact_episode_count = sum(
        1
        for session in swapped_holdouts
        for episode in session.episodes
        if episode.category == "real_swing"
        and episode.audio_character == "quiet"
        and episode.audio_expectation == "must_trigger"
    )
    quiet_real_impact_session_count = sum(
        any(
            episode.category == "real_swing"
            and episode.audio_character == "quiet"
            and episode.audio_expectation == "must_trigger"
            for episode in session.episodes
        )
        for session in swapped_holdouts
    )
    quiet_real_impact_episode_deficit = max(
        0, MINIMUM_QUIET_REAL_IMPACT_EPISODES - quiet_real_impact_episode_count
    )
    quiet_real_impact_session_deficit = max(
        0,
        MINIMUM_DISTINCT_QUIET_REAL_IMPACT_SESSIONS - quiet_real_impact_session_count,
    )
    quiet_real_impact = quiet_real_impact_episode_count > 0
    swapped_recording_ids = sorted(session.recording_id for session in swapped_holdouts)
    swapped_session_deficit = max(
        0,
        MINIMUM_PHONE_VIEW_SWAPPED_HOLDOUT_SESSIONS - len(swapped_recording_ids),
    )
    missing_pose = sorted(category for category, count in pose_episode_counts.items() if count == 0)
    missing_audio = sorted(
        category for category, count in audio_episode_counts.items() if count == 0
    )
    insufficient_pose = sorted(
        category
        for category in REQUIRED_POSE_CATEGORIES
        if pose_episode_deficits[category] or pose_session_deficits[category]
    )
    insufficient_audio = sorted(
        category
        for category in REQUIRED_AUDIO_NEGATIVE_CATEGORIES
        if audio_episode_deficits[category] or audio_session_deficits[category]
    )
    diversity = {
        "golfer": sorted({session.scene.golfer_label for session in swapped_holdouts}),
        "environment": sorted({session.scene.environment_label for session in swapped_holdouts}),
        "lighting": sorted({session.scene.lighting_label for session in swapped_holdouts}),
        "framing": sorted({session.scene.framing_label for session in swapped_holdouts}),
    }
    missing_pose_diversity = sorted(
        label
        for label in ("golfer", "lighting", "framing")
        if len(diversity[label]) < MINIMUM_POSE_DIVERSITY_VALUES
    )
    gaps: list[str] = []
    if not holdouts:
        gaps.append("no validation/challenge sessions")
    for session in holdouts:
        if session.review.state != "complete":
            gaps.append(f"{session.recording_id}: review state is {session.review.state}")
        for role in ROLES:
            stream = session.streams[role]
            if not _full_review_coverage(stream):
                gaps.append(f"{session.recording_id}/{role}: timeline review is incomplete")
            if not _assets_pinned(stream):
                gaps.append(f"{session.recording_id}/{role}: assets are not fully hash-pinned")
    if swapped_session_deficit:
        gap = "insufficient fully reviewed phone/view-swapped holdout sessions: "
        gap += f"observed {len(swapped_recording_ids)}, requires "
        gap += f"{MINIMUM_PHONE_VIEW_SWAPPED_HOLDOUT_SESSIONS}"
        gaps.append(gap)
    if missing_pose:
        gaps.append("missing pose categories: " + ", ".join(missing_pose))
    if missing_audio:
        gaps.append("missing audio-negative categories: " + ", ".join(missing_audio))
    if insufficient_pose:
        gaps.append("under-repeated pose categories: " + ", ".join(insufficient_pose))
    if insufficient_audio:
        gaps.append("under-repeated audio-negative categories: " + ", ".join(insufficient_audio))
    if not quiet_real_impact:
        gaps.append("no reviewed quiet real impact")
    elif quiet_real_impact_episode_deficit or quiet_real_impact_session_deficit:
        gap = f"under-repeated quiet real impacts: episodes {quiet_real_impact_episode_count}/"
        gap += f"{MINIMUM_QUIET_REAL_IMPACT_EPISODES}, sessions "
        gap += f"{quiet_real_impact_session_count}/"
        gap += f"{MINIMUM_DISTINCT_QUIET_REAL_IMPACT_SESSIONS}"
        gaps.append(gap)
    if missing_pose_diversity:
        gaps.append("insufficient pose diversity: " + ", ".join(missing_pose_diversity))
    if not assets_verified:
        gaps.append("asset hashes have not been verified against a media root")
    common_ready = (
        bool(holdouts)
        and len(reviewed_holdouts) == len(holdouts)
        and not swapped_session_deficit
        and assets_verified
    )
    next_collection_plan = _collection_plan(
        corpus.development_device_assignment,
        missing_pose=insufficient_pose,
        missing_audio=insufficient_audio,
        pose_episode_deficits=pose_episode_deficits,
        pose_session_deficits=pose_session_deficits,
        audio_episode_deficits=audio_episode_deficits,
        audio_session_deficits=audio_session_deficits,
        quiet_real_impact_episode_deficit=quiet_real_impact_episode_deficit,
        quiet_real_impact_session_deficit=quiet_real_impact_session_deficit,
        swapped_session_deficit=swapped_session_deficit,
        diversity=diversity,
    )
    return {
        "schema_version": SCHEMA_VERSION,
        "readiness_policy": _readiness_policy(),
        "corpus_id": corpus.corpus_id,
        "session_count": len(corpus.sessions),
        "holdout_session_count": len(holdouts),
        "fully_reviewed_holdout_count": len(reviewed_holdouts),
        "fully_reviewed_holdout_ids": [session.recording_id for session in reviewed_holdouts],
        "phone_view_swapped_holdout_ids": swapped_recording_ids,
        "observed_categories": sorted(categories),
        "diversity": diversity,
        "missing_pose_diversity": missing_pose_diversity,
        "missing_pose_categories": missing_pose,
        "missing_audio_negative_categories": missing_audio,
        "insufficient_pose_categories": insufficient_pose,
        "insufficient_audio_negative_categories": insufficient_audio,
        "pose_category_episode_counts": pose_episode_counts,
        "pose_category_distinct_session_counts": pose_session_counts,
        "audio_negative_category_episode_counts": audio_episode_counts,
        "audio_negative_category_distinct_session_counts": audio_session_counts,
        "quiet_real_impact_present": quiet_real_impact,
        "quiet_real_impact_episode_count": quiet_real_impact_episode_count,
        "quiet_real_impact_distinct_session_count": quiet_real_impact_session_count,
        "asset_verification_performed": assets_verified,
        "pose_release_gate_ready": (
            common_ready and not insufficient_pose and not missing_pose_diversity
        ),
        "audio_detector_selection_ready": (
            common_ready
            and not insufficient_audio
            and not quiet_real_impact_episode_deficit
            and not quiet_real_impact_session_deficit
        ),
        "next_collection_plan": next_collection_plan,
        "gaps": gaps,
    }


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def validate_assets(  # noqa: C901, PLR0912
    corpus: Corpus, media_root: Path
) -> tuple[dict[str, str], ...]:
    """Verify every declared asset and the identity/role of source field manifests."""
    root = media_root.resolve(strict=True)
    results: list[dict[str, str]] = []
    for session in corpus.sessions:
        shared_ids: set[str] = set()
        for role in ROLES:
            stream = session.streams[role]
            resolved_assets: dict[str, Path] = {}
            for kind, asset in (
                ("source_manifest", stream.source_manifest),
                ("video", stream.video),
                ("audio", stream.audio),
            ):
                path = (root / Path(*asset.path.parts)).resolve(strict=True)
                if not path.is_relative_to(root) or not path.is_file():
                    message = f"{session.recording_id}/{role}/{kind} is not a root-bounded file"
                    raise ValueError(message)
                digest = _sha256(path)
                if asset.sha256 is None or digest != asset.sha256:
                    message = f"{session.recording_id}/{role}/{kind} SHA-256 mismatch or unpinned"
                    raise ValueError(message)
                resolved_assets[kind] = path
                results.append(
                    {
                        "recording_id": session.recording_id,
                        "role": role,
                        "kind": kind,
                        "path": asset.path.as_posix(),
                        "sha256": digest,
                    }
                )
            manifest_path = root / Path(*stream.source_manifest.path.parts)
            source = _object(
                _decode_json(manifest_path.read_text(encoding="utf-8")), "source manifest"
            )
            if source.get("schema_version") != 1 or source.get("session_kind") != "field_recording":
                message = f"{session.recording_id}/{role} source is not a field recording manifest"
                raise ValueError(message)
            if source.get("role") != role or source.get("node_id") != stream.node_id:
                message = f"{session.recording_id}/{role} source identity or role disagrees"
                raise ValueError(message)
            duration_us = source.get("duration_us")
            if not isinstance(duration_us, str) or not duration_us.isdecimal():
                message = f"{session.recording_id}/{role} source duration is invalid"
                raise ValueError(message)
            if int(duration_us) // 1000 != stream.duration_ms:
                message = f"{session.recording_id}/{role} source duration disagrees"
                raise ValueError(message)
            for kind in ("video", "audio"):
                metadata = _object(source.get(kind), f"{session.recording_id}/{role} {kind}")
                declared_bytes = metadata.get("bytes")
                if not isinstance(declared_bytes, str) or not declared_bytes.isdecimal():
                    message = f"{session.recording_id}/{role} source {kind} byte count is invalid"
                    raise ValueError(message)
                if int(declared_bytes) != resolved_assets[kind].stat().st_size:
                    message = f"{session.recording_id}/{role} source {kind} byte count disagrees"
                    raise ValueError(message)
            shared = source.get("shared_recording_id")
            if not isinstance(shared, str) or not shared:
                message = f"{session.recording_id}/{role} source shared recording ID is invalid"
                raise ValueError(message)
            shared_ids.add(shared)
        if shared_ids != {session.recording_id}:
            message = f"{session.recording_id} source pair does not share the declared recording ID"
            raise ValueError(message)
    return tuple(results)


def _asset_for(path: Path, root: Path) -> dict[str, str]:
    resolved = path.resolve(strict=True)
    if not resolved.is_relative_to(root) or not resolved.is_file():
        message = f"asset {path} must be a regular file beneath --media-root"
        raise ValueError(message)
    return {"path": resolved.relative_to(root).as_posix(), "sha256": _sha256(resolved)}


def _source_summary(path: Path, role: str) -> tuple[dict[str, object], int, str, str]:
    decoded = _object(_decode_json(path.read_text(encoding="utf-8")), f"{role} source manifest")
    if decoded.get("schema_version") != 1 or decoded.get("session_kind") != "field_recording":
        message = f"{role} source is not a field recording manifest"
        raise ValueError(message)
    if decoded.get("role") != role:
        message = f"{role} source manifest has the wrong role"
        raise ValueError(message)
    duration_us = decoded.get("duration_us")
    node_id = decoded.get("node_id")
    recording_id = decoded.get("shared_recording_id")
    if not isinstance(duration_us, str) or not duration_us.isdecimal():
        message = f"{role} source manifest duration is invalid"
        raise ValueError(message)
    if (
        not isinstance(node_id, str)
        or not node_id
        or not isinstance(recording_id, str)
        or not recording_id
    ):
        message = f"{role} source manifest identity is invalid"
        raise ValueError(message)
    return decoded, int(duration_us) // 1000, node_id, recording_id


def create_session_skeleton(  # noqa: PLR0913
    *,
    corpus_id: str,
    development_recording_ids: Sequence[str],
    development_assignment: Mapping[str, str],
    split: str,
    media_root: Path,
    source_manifests: Mapping[str, Path],
    videos: Mapping[str, Path],
    audio: Mapping[str, Path],
    device_assignment: Mapping[str, str],
    scene: Mapping[str, str],
) -> dict[str, object]:
    """Pin a raw pair while intentionally leaving every semantic label unreviewed."""
    root = media_root.resolve(strict=True)
    streams: dict[str, object] = {}
    recording_ids: set[str] = set()
    for role in ROLES:
        _, duration_ms, node_id, recording_id = _source_summary(source_manifests[role], role)
        recording_ids.add(recording_id)
        streams[role] = {
            "node_id": node_id,
            "device_label": device_assignment[role],
            "duration_ms": duration_ms,
            "source_manifest": _asset_for(source_manifests[role], root),
            "video": _asset_for(videos[role], root),
            "audio": _asset_for(audio[role], root),
            "reviewed_intervals": [],
        }
    if len(recording_ids) != 1:
        message = "source manifests do not share one recording ID"
        raise ValueError(message)
    recording_id = next(iter(recording_ids))
    value: dict[str, object] = {
        "schema_version": SCHEMA_VERSION,
        "corpus_id": corpus_id,
        "development_recording_ids": list(development_recording_ids),
        "development_device_assignment": dict(development_assignment),
        "sessions": [
            {
                "recording_id": recording_id,
                "split": split,
                "scene": dict(scene),
                "streams": streams,
                "review": {"state": "needs_review", "reviewer": None, "reviewed_at_utc": None},
                "episodes": [],
                "notes": (
                    "Generated acquisition skeleton; no semantic ground truth has been assigned."
                ),
            }
        ],
    }
    parsed = parse_manifest(json.dumps(value))
    validate_assets(parsed, root)
    return value


def append_session_skeleton(base_text: str, skeleton: Mapping[str, object]) -> dict[str, object]:
    """Append one generated session while preserving corpus provenance exactly."""
    base = _object(_decode_json(base_text), "base manifest")
    candidate = _object(_decode_json(json.dumps(skeleton)), "session skeleton")
    identity_fields = (
        "schema_version",
        "corpus_id",
        "development_recording_ids",
        "development_device_assignment",
    )
    for field in identity_fields:
        if base.get(field) != candidate.get(field):
            message = f"cannot append a session with different {field}"
            raise ValueError(message)
    base_sessions = _array(base.get("sessions"), "base sessions")
    candidate_sessions = _array(candidate.get("sessions"), "candidate sessions")
    if len(candidate_sessions) != 1:
        message = "a generated session skeleton must contain exactly one session"
        raise ValueError(message)
    base_sessions.append(candidate_sessions[0])
    parse_manifest(json.dumps(base))
    return base


def _render(value: object, output: Path | None) -> None:
    rendered = json.dumps(value, indent=2, sort_keys=True) + "\n"
    if output is None:
        print(rendered, end="")
        return
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=output.parent,
            prefix=f".{output.name}.",
            suffix=".tmp",
            delete=False,
        ) as temporary:
            temporary.write(rendered)
            temporary.flush()
            os.fsync(temporary.fileno())
            temporary_path = Path(temporary.name)
        temporary_path.replace(output)
        temporary_path = None
        directory = os.open(output.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def _paths_alias(first: Path, second: Path) -> bool:
    """Detect lexical, symlink, and existing-hardlink output/input aliases."""
    try:
        if first.exists() and second.exists() and first.samefile(second):
            return True
    except OSError:
        pass
    return first.resolve(strict=False) == second.resolve(strict=False)


def _reject_output_alias(output: Path | None, inputs: Sequence[Path]) -> None:
    if output is not None and any(_paths_alias(output, path) for path in inputs):
        message = "--output must not replace an input manifest or media asset"
        raise ValueError(message)


@final
class _Arguments(argparse.Namespace):
    def __init__(self) -> None:
        """Initialize typed defaults that the selected argparse subcommand replaces."""
        super().__init__()
        self.command = ""
        self.development_down_the_line_device = ""
        self.development_face_on_device = ""
        self.output: Path | None = None
        self.corpus_id = ""
        self.development_recording_id: list[str] = []
        self.down_the_line_device = ""
        self.face_on_device = ""
        self.golfer_label = ""
        self.environment_label = ""
        self.lighting_label = ""
        self.framing_label = ""
        self.append_to: Path | None = None
        self.split = ""
        self.media_root: Path | None = None
        self.down_the_line_manifest = Path()
        self.face_on_manifest = Path()
        self.down_the_line_video = Path()
        self.face_on_video = Path()
        self.down_the_line_audio = Path()
        self.face_on_audio = Path()
        self.manifest = Path()


def _role_paths(arguments: _Arguments, prefix: str) -> dict[str, Path]:
    return {
        "down_the_line": cast("Path", getattr(arguments, f"down_the_line_{prefix}")),
        "face_on": cast("Path", getattr(arguments, f"face_on_{prefix}")),
    }


def _parse_arguments(arguments: Sequence[str] | None) -> _Arguments:
    parser = argparse.ArgumentParser(description="create or audit paired field evidence")
    subparsers = parser.add_subparsers(dest="command", required=True)
    plan = subparsers.add_parser("plan", help="emit the checklist for the first holdout")
    plan.add_argument("--development-down-the-line-device", required=True)
    plan.add_argument("--development-face-on-device", required=True)
    plan.add_argument("--output", type=Path)
    create = subparsers.add_parser("create", help="hash-pin a pair as an unreviewed skeleton")
    create.add_argument("--corpus-id", required=True)
    create.add_argument("--development-recording-id", action="append", default=[])
    create.add_argument("--development-down-the-line-device", required=True)
    create.add_argument("--development-face-on-device", required=True)
    create.add_argument("--down-the-line-device", required=True)
    create.add_argument("--face-on-device", required=True)
    create.add_argument("--golfer-label", required=True)
    create.add_argument("--environment-label", required=True)
    create.add_argument("--lighting-label", required=True)
    create.add_argument("--framing-label", required=True)
    create.add_argument("--append-to", type=Path)
    create.add_argument("--split", choices=sorted(SPLITS - {"development"}), default="validation")
    create.add_argument("--media-root", type=Path, required=True)
    for role in ROLES:
        option = role.replace("_", "-")
        create.add_argument(
            f"--{option}-manifest", dest=f"{role}_manifest", type=Path, required=True
        )
        create.add_argument(f"--{option}-video", dest=f"{role}_video", type=Path, required=True)
        create.add_argument(f"--{option}-audio", dest=f"{role}_audio", type=Path, required=True)
    create.add_argument("--output", type=Path)
    validate = subparsers.add_parser("validate", help="validate labels and emit readiness gaps")
    validate.add_argument("--manifest", type=Path, required=True)
    validate.add_argument("--media-root", type=Path)
    validate.add_argument("--output", type=Path)
    return parser.parse_args(arguments, namespace=_Arguments())


def main(arguments: Sequence[str] | None = None) -> int:
    """CLI entry point."""
    parsed = _parse_arguments(arguments)
    if parsed.command == "plan":
        value = initial_collection_plan(
            {
                "down_the_line": parsed.development_down_the_line_device,
                "face_on": parsed.development_face_on_device,
            }
        )
        _render(value, parsed.output)
        return 0
    if parsed.command == "create":
        if parsed.media_root is None:
            message = "argparse requires --media-root for create"
            raise AssertionError(message)
        media_root = parsed.media_root
        media_inputs = tuple(
            path
            for prefix in ("manifest", "video", "audio")
            for path in _role_paths(parsed, prefix).values()
        )
        permitted_in_place_append = (
            parsed.append_to is not None
            and parsed.output is not None
            and _paths_alias(parsed.output, parsed.append_to)
        )
        _reject_output_alias(parsed.output, media_inputs)
        if parsed.append_to is not None and not permitted_in_place_append:
            _reject_output_alias(parsed.output, (parsed.append_to,))
        value = create_session_skeleton(
            corpus_id=parsed.corpus_id,
            development_recording_ids=parsed.development_recording_id,
            development_assignment={
                "down_the_line": parsed.development_down_the_line_device,
                "face_on": parsed.development_face_on_device,
            },
            split=parsed.split,
            media_root=media_root,
            source_manifests=_role_paths(parsed, "manifest"),
            videos=_role_paths(parsed, "video"),
            audio=_role_paths(parsed, "audio"),
            device_assignment={
                "down_the_line": parsed.down_the_line_device,
                "face_on": parsed.face_on_device,
            },
            scene={
                "golfer_label": parsed.golfer_label,
                "environment_label": parsed.environment_label,
                "lighting_label": parsed.lighting_label,
                "framing_label": parsed.framing_label,
            },
        )
        if parsed.append_to is not None:
            value = append_session_skeleton(parsed.append_to.read_text(encoding="utf-8"), value)
            validate_assets(parse_manifest(json.dumps(value)), media_root)
        _render(value, parsed.output)
        return 0
    _reject_output_alias(parsed.output, (parsed.manifest,))
    corpus = parse_manifest(parsed.manifest.read_text(encoding="utf-8"))
    assets = validate_assets(corpus, parsed.media_root) if parsed.media_root is not None else ()
    report = readiness_report(corpus, assets_verified=parsed.media_root is not None)
    report["verified_assets"] = list(assets)
    _render(report, parsed.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
