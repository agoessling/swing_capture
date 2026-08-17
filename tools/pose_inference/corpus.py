"""Strict metadata contract for the private pose-trigger evaluation corpus."""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import itertools
import json
import re
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, cast

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence


SCHEMA_VERSION = 1
SUPPORTED_VIEWS = frozenset({"dtl", "atl"})
VIEWS = SUPPORTED_VIEWS | {"unsupported_45_degree", "unsupported_front", "unknown"}
CAPTURE_ROLES = frozenset({"down_the_line", "face_on"})
DISPOSITIONS = frozenset(
    {
        "primary_positive",
        "secondary_positive",
        "geometry_rejection",
        "transition_only",
        "diagnostic",
    }
)
SPLITS = frozenset({"development", "validation", "challenge"})
POSITIVE_DISPOSITIONS = frozenset({"primary_positive", "secondary_positive"})
CLIP_ID = re.compile(r"[A-Z][A-Z0-9]{2,15}")
SHA256_HEX_LENGTH = 64
SOURCE_TRIM_FIELD_COUNT = 2
MINIMUM_MEDIA_PATH_PARTS = 2
HITTING_REGION_FIELD_COUNT = 4


@dataclasses.dataclass(frozen=True)
class Interval:
    """One half-open interval in clip-relative milliseconds."""

    start_ms: int
    end_ms: int
    reason: str


@dataclasses.dataclass(frozen=True)
class Labels:
    """Human timing labels, all relative to the beginning of the trimmed clip."""

    safe_arm_start_ms: int | None
    preferred_arm_ms: int | None
    takeaway_ms: int | None
    impact_ms: int | None
    reference_arm_ms: int | None
    must_not_arm: tuple[Interval, ...]


@dataclasses.dataclass(frozen=True)
class HittingRegion:
    """Inclusive normalized station rectangle used by the pose feature adapter."""

    left: float
    top: float
    right: float
    bottom: float

    def encoded(self) -> str:
        """Return the Java CLI's stable left,top,right,bottom representation."""
        values = (self.left, self.top, self.right, self.bottom)
        return ",".join(format(value, ".9g") for value in values)


@dataclasses.dataclass(frozen=True)
class Clip:
    """One source-grouped evaluation clip without the media payload."""

    clip_id: str
    title: str
    creator: str
    source_url: str
    source_group: str
    source_start_ms: int
    source_end_ms: int
    media_path: PurePosixPath
    media_sha256: str | None
    hitting_region: HittingRegion | None
    view: str
    capture_role: str | None
    disposition: str
    split: str
    labels: Labels
    notes: str

    @property
    def duration_ms(self) -> int:
        """Return the half-open trimmed duration."""
        return self.source_end_ms - self.source_start_ms


@dataclasses.dataclass(frozen=True)
class Corpus:
    """Validated corpus metadata."""

    sample_period_ms: int
    clips: tuple[Clip, ...]


def _object(value: object, name: str) -> dict[str, object]:
    if not isinstance(value, dict):
        message = f"{name} must be an object"
        raise TypeError(message)
    encoded = cast("dict[object, object]", value)
    if not all(isinstance(key, str) for key in encoded):
        message = f"{name} keys must be strings"
        raise TypeError(message)
    return cast("dict[str, object]", encoded)


def _exact_keys(value: Mapping[str, object], expected: set[str], name: str) -> None:
    actual = set(value)
    if actual != expected:
        message = (
            f"{name} keys differ: missing={sorted(expected - actual)} "
            f"unknown={sorted(actual - expected)}"
        )
        raise ValueError(message)


def _string(value: object, name: str) -> str:
    if not isinstance(value, str) or not value:
        message = f"{name} must be a nonempty string"
        raise TypeError(message)
    return value


def _integer(value: object, name: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        message = f"{name} must be an integer"
        raise TypeError(message)
    return value


def _optional_integer(value: object, name: str) -> int | None:
    return None if value is None else _integer(value, name)


def _number(value: object, name: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        message = f"{name} must be a number"
        raise TypeError(message)
    parsed = float(value)
    if not 0.0 <= parsed <= 1.0:
        message = f"{name} must be in [0, 1]"
        raise ValueError(message)
    return parsed


def _enum(value: object, allowed: frozenset[str], name: str) -> str:
    parsed = _string(value, name)
    if parsed not in allowed:
        message = f"{name} has unsupported value {parsed!r}"
        raise ValueError(message)
    return parsed


def _parse_interval(value: object, index: int, duration_ms: int) -> Interval:
    name = f"must_not_arm[{index}]"
    encoded = _object(value, name)
    _exact_keys(encoded, {"start_ms", "end_ms", "reason"}, name)
    interval = Interval(
        start_ms=_integer(encoded["start_ms"], f"{name}.start_ms"),
        end_ms=_integer(encoded["end_ms"], f"{name}.end_ms"),
        reason=_string(encoded["reason"], f"{name}.reason"),
    )
    if interval.start_ms < 0 or interval.end_ms <= interval.start_ms:
        message = f"{name} must be a nonempty nonnegative half-open interval"
        raise ValueError(message)
    if interval.end_ms > duration_ms:
        message = f"{name} exceeds the trimmed clip duration"
        raise ValueError(message)
    return interval


def _parse_labels(value: object, duration_ms: int) -> Labels:
    encoded = _object(value, "labels")
    _exact_keys(
        encoded,
        {
            "safe_arm_start_ms",
            "preferred_arm_ms",
            "takeaway_ms",
            "impact_ms",
            "reference_arm_ms",
            "must_not_arm",
        },
        "labels",
    )
    raw_intervals = encoded["must_not_arm"]
    if not isinstance(raw_intervals, list):
        message = "labels.must_not_arm must be an array"
        raise TypeError(message)
    typed_intervals = cast("list[object]", raw_intervals)
    intervals = tuple(
        _parse_interval(interval, index, duration_ms)
        for index, interval in enumerate(typed_intervals)
    )
    for previous, current in itertools.pairwise(intervals):
        if current.start_ms < previous.end_ms:
            message = "must_not_arm intervals must be sorted and nonoverlapping"
            raise ValueError(message)
    labels = Labels(
        safe_arm_start_ms=_optional_integer(encoded["safe_arm_start_ms"], "safe_arm_start_ms"),
        preferred_arm_ms=_optional_integer(encoded["preferred_arm_ms"], "preferred_arm_ms"),
        takeaway_ms=_optional_integer(encoded["takeaway_ms"], "takeaway_ms"),
        impact_ms=_optional_integer(encoded["impact_ms"], "impact_ms"),
        reference_arm_ms=_optional_integer(encoded["reference_arm_ms"], "reference_arm_ms"),
        must_not_arm=intervals,
    )
    for field in dataclasses.fields(Labels):
        if field.name == "must_not_arm":
            continue
        timestamp = cast("int | None", getattr(labels, field.name))
        if timestamp is not None and not 0 <= timestamp < duration_ms:
            message = f"labels.{field.name} must fall within the trimmed clip"
            raise ValueError(message)
    return labels


def _parse_source_trim(value: object) -> tuple[int, int]:
    if not isinstance(value, list):
        message = "source_trim_ms must contain exactly two integers"
        raise TypeError(message)
    encoded = cast("list[object]", value)
    if len(encoded) != SOURCE_TRIM_FIELD_COUNT:
        message = "source_trim_ms must contain exactly two integers"
        raise TypeError(message)
    start_ms = _integer(encoded[0], "source_trim_ms[0]")
    end_ms = _integer(encoded[1], "source_trim_ms[1]")
    if start_ms < 0 or end_ms <= start_ms:
        message = "source_trim_ms must be a nonempty nonnegative half-open interval"
        raise ValueError(message)
    return start_ms, end_ms


def _parse_media(value: object) -> tuple[PurePosixPath, str | None]:
    encoded = _object(value, "media")
    _exact_keys(encoded, {"path", "sha256"}, "media")
    path = PurePosixPath(_string(encoded["path"], "media.path"))
    if path.is_absolute() or ".." in path.parts or len(path.parts) < MINIMUM_MEDIA_PATH_PARTS:
        message = "media.path must be a safe relative path beneath a corpus root"
        raise ValueError(message)
    raw_sha256 = encoded["sha256"]
    sha256 = None if raw_sha256 is None else _string(raw_sha256, "media.sha256").lower()
    if sha256 is not None and (
        len(sha256) != SHA256_HEX_LENGTH
        or any(character not in "0123456789abcdef" for character in sha256)
    ):
        message = "media.sha256 must be lowercase hexadecimal SHA-256"
        raise ValueError(message)
    return path, sha256


def _parse_hitting_region(value: object) -> HittingRegion | None:
    if value is None:
        return None
    if not isinstance(value, list):
        message = "hitting_region must contain left, top, right, bottom or be null"
        raise TypeError(message)
    encoded = cast("list[object]", value)
    if len(encoded) != HITTING_REGION_FIELD_COUNT:
        message = "hitting_region must contain left, top, right, bottom or be null"
        raise TypeError(message)
    region = HittingRegion(
        left=_number(encoded[0], "hitting_region[0]"),
        top=_number(encoded[1], "hitting_region[1]"),
        right=_number(encoded[2], "hitting_region[2]"),
        bottom=_number(encoded[3], "hitting_region[3]"),
    )
    if region.right <= region.left or region.bottom <= region.top:
        message = "hitting_region must have positive width and height"
        raise ValueError(message)
    return region


def _validate_clip_semantics(clip: Clip) -> None:
    expected_role = {"dtl": "down_the_line", "atl": "face_on"}.get(clip.view)
    if clip.capture_role != expected_role:
        message = f"{clip.clip_id} view and capture_role disagree"
        raise ValueError(message)
    labels = clip.labels
    if clip.disposition in POSITIVE_DISPOSITIONS:
        _validate_positive_clip(clip)
    elif labels.safe_arm_start_ms is not None or labels.preferred_arm_ms is not None:
        message = f"{clip.clip_id} non-positive clips cannot define an arm window"
        raise ValueError(message)
    if clip.disposition == "geometry_rejection" and (
        clip.view in SUPPORTED_VIEWS or labels.reference_arm_ms is None
    ):
        message = f"{clip.clip_id} geometry rejection requires an unsupported view and reference"
        raise ValueError(message)


def _validate_positive_clip(clip: Clip) -> None:
    labels = clip.labels
    if clip.view not in SUPPORTED_VIEWS:
        message = f"{clip.clip_id} positive clips require a supported station view"
        raise ValueError(message)
    if clip.hitting_region is None:
        message = f"{clip.clip_id} positive clips require a configured hitting region"
        raise ValueError(message)
    required = (labels.safe_arm_start_ms, labels.preferred_arm_ms, labels.takeaway_ms)
    if any(value is None for value in required):
        message = f"{clip.clip_id} positive clips require safe, preferred, and takeaway labels"
        raise ValueError(message)
    safe, preferred, takeaway = cast("tuple[int, int, int]", required)
    if not safe <= preferred < takeaway:
        message = f"{clip.clip_id} must satisfy safe <= preferred < takeaway"
        raise ValueError(message)
    if labels.reference_arm_ms is not None:
        message = f"{clip.clip_id} positive clips cannot use reference_arm_ms"
        raise ValueError(message)
    if any(interval.start_ms <= preferred < interval.end_ms for interval in labels.must_not_arm):
        message = f"{clip.clip_id} preferred arm falls in a must-not-arm interval"
        raise ValueError(message)


def _parse_clip(value: object) -> Clip:
    encoded = _object(value, "clip")
    _exact_keys(
        encoded,
        {
            "id",
            "title",
            "creator",
            "source_url",
            "source_group",
            "source_trim_ms",
            "media",
            "hitting_region",
            "view",
            "capture_role",
            "disposition",
            "split",
            "labels",
            "notes",
        },
        "clip",
    )
    clip_id = _string(encoded["id"], "clip.id")
    if CLIP_ID.fullmatch(clip_id) is None:
        message = "clip.id must be 3-16 uppercase alphanumeric characters"
        raise ValueError(message)
    source_url = _string(encoded["source_url"], f"{clip_id}.source_url")
    if not source_url.startswith("https://"):
        message = f"{clip_id}.source_url must use HTTPS"
        raise ValueError(message)
    source_start_ms, source_end_ms = _parse_source_trim(encoded["source_trim_ms"])
    media_path, media_sha256 = _parse_media(encoded["media"])
    raw_capture_role = encoded["capture_role"]
    capture_role = (
        None
        if raw_capture_role is None
        else _enum(raw_capture_role, CAPTURE_ROLES, f"{clip_id}.capture_role")
    )
    clip = Clip(
        clip_id=clip_id,
        title=_string(encoded["title"], f"{clip_id}.title"),
        creator=_string(encoded["creator"], f"{clip_id}.creator"),
        source_url=source_url,
        source_group=_string(encoded["source_group"], f"{clip_id}.source_group"),
        source_start_ms=source_start_ms,
        source_end_ms=source_end_ms,
        media_path=media_path,
        media_sha256=media_sha256,
        hitting_region=_parse_hitting_region(encoded["hitting_region"]),
        view=_enum(encoded["view"], VIEWS, f"{clip_id}.view"),
        capture_role=capture_role,
        disposition=_enum(encoded["disposition"], DISPOSITIONS, f"{clip_id}.disposition"),
        split=_enum(encoded["split"], SPLITS, f"{clip_id}.split"),
        labels=_parse_labels(encoded["labels"], source_end_ms - source_start_ms),
        notes=_string(encoded["notes"], f"{clip_id}.notes"),
    )
    _validate_clip_semantics(clip)
    return clip


def parse_manifest(text: str) -> Corpus:
    """Parse and validate a complete corpus manifest."""
    decoded = cast("object", json.loads(text))
    root = _object(decoded, "manifest")
    _exact_keys(root, {"schema_version", "sample_period_ms", "clips"}, "manifest")
    if _integer(root["schema_version"], "schema_version") != SCHEMA_VERSION:
        message = "unsupported corpus schema_version"
        raise ValueError(message)
    sample_period_ms = _integer(root["sample_period_ms"], "sample_period_ms")
    if sample_period_ms <= 0:
        message = "sample_period_ms must be positive"
        raise ValueError(message)
    raw_clips = root["clips"]
    if not isinstance(raw_clips, list) or not raw_clips:
        message = "clips must be a nonempty array"
        raise TypeError(message)
    clips = tuple(_parse_clip(value) for value in cast("list[object]", raw_clips))
    ids = [clip.clip_id for clip in clips]
    if len(ids) != len(set(ids)):
        message = "clip ids must be unique"
        raise ValueError(message)
    groups: dict[str, str] = {}
    for clip in clips:
        prior = groups.setdefault(clip.source_group, clip.split)
        if prior != clip.split:
            message = f"source group {clip.source_group!r} crosses corpus splits"
            raise ValueError(message)
    return Corpus(sample_period_ms=sample_period_ms, clips=clips)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def validate_media(corpus: Corpus, media_root: Path) -> tuple[dict[str, str], ...]:
    """Resolve all private media beneath one root and verify pinned hashes when present."""
    root = media_root.resolve(strict=True)
    results: list[dict[str, str]] = []
    for clip in corpus.clips:
        path = (root / Path(*clip.media_path.parts)).resolve(strict=True)
        if not path.is_relative_to(root) or not path.is_file():
            message = f"{clip.clip_id} media does not resolve to a regular file beneath root"
            raise ValueError(message)
        sha256 = _sha256(path)
        if clip.media_sha256 is not None and sha256 != clip.media_sha256:
            message = f"{clip.clip_id} media SHA-256 mismatch"
            raise ValueError(message)
        results.append({"id": clip.clip_id, "path": clip.media_path.as_posix(), "sha256": sha256})
    return tuple(results)


def summarize(corpus: Corpus, media: tuple[dict[str, str], ...] = ()) -> dict[str, object]:
    """Return a deterministic machine-readable inventory."""
    by_split = dict.fromkeys(sorted(SPLITS), 0)
    by_view = dict.fromkeys(sorted(VIEWS), 0)
    by_disposition = dict.fromkeys(sorted(DISPOSITIONS), 0)
    for clip in corpus.clips:
        by_split[clip.split] += 1
        by_view[clip.view] += 1
        by_disposition[clip.disposition] += 1
    return {
        "schema_version": SCHEMA_VERSION,
        "sample_period_ms": corpus.sample_period_ms,
        "clip_count": len(corpus.clips),
        "source_group_count": len({clip.source_group for clip in corpus.clips}),
        "by_split": by_split,
        "by_view": by_view,
        "by_disposition": by_disposition,
        "media": list(media),
    }


def main(arguments: Sequence[str] | None = None) -> int:
    """Validate corpus metadata and optionally its separately stored private media."""
    parser = argparse.ArgumentParser(description="validate pose-trigger corpus metadata")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--media-root", type=Path)
    parser.add_argument("--output", type=Path)
    parsed = parser.parse_args(arguments)
    manifest_path = cast("Path", parsed.manifest)
    media_root = cast("Path | None", parsed.media_root)
    output_path = cast("Path | None", parsed.output)
    corpus = parse_manifest(manifest_path.read_text(encoding="utf-8"))
    media = validate_media(corpus, media_root) if media_root is not None else ()
    rendered = json.dumps(summarize(corpus, media), indent=2, sort_keys=True) + "\n"
    if output_path is None:
        print(rendered, end="")
    else:
        output_path.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
