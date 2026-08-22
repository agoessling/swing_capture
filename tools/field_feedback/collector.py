"""Authenticated, deduplicating collection of Android diagnostic archives."""

# Validation failures are deliberately reported at the check site with evidence-specific text.
# ruff: noqa: EM101, EM102, TRY003

from __future__ import annotations

import dataclasses
import datetime
import hashlib
import io
import json
import math
import os
import re
import stat
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from collections.abc import Callable, Iterable, Mapping, Sequence
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Protocol, cast, final

if TYPE_CHECKING:
    from email.message import Message

INDEX_SCHEMA_VERSION = 1
NODE_SCHEMA_VERSION = 1
SESSION_SCHEMA_VERSION = 1
EXPORT_SCHEMA_VERSION = 1
INCIDENT_SCHEMA_VERSION = 1
MAXIMUM_ARCHIVE_BYTES = 160 * 1024 * 1024
MAXIMUM_EXPANDED_BYTES = 128 * 1024 * 1024
MAXIMUM_ARCHIVE_ENTRIES = 129
MINIMUM_ARCHIVE_ENTRIES = 3
MAXIMUM_FEEDBACK_NOTE_LENGTH = 500
MAXIMUM_FEEDBACK_NOTE_UTF8_BYTES = 2_048
MAXIMUM_TIMING_MARKS = 32
MAXIMUM_PREVIEW_OBSERVATIONS = 300
MAXIMUM_PREVIEW_MODEL_ID_UTF8_BYTES = 128
MAXIMUM_PREVIEW_REASON_UTF8_BYTES = 512
MINIMUM_JPEG_BYTES = 4
MAXIMUM_PENDING_TRANSACTIONS = 128
MAXIMUM_TRANSACTION_BYTES = 256 * 1024
DUAL_NODE_COUNT = 2
STANDBY_AUDIO_SAMPLE_RATE_HZ = 48_000
PCM16_WAVE_FORMAT_CHUNK_BYTES = 16
PCM16_MONO_BLOCK_ALIGN_BYTES = 2
PCM16_BITS_PER_SAMPLE = 16
PREVIEW_MJPEG_NAME = "preview_frames.mjpeg"
POSE_TRACE_NAME = "pose_trace.ndjson"
STANDBY_SESSION_KIND = "standby_diagnostic"
CAPTURE_SESSION_KIND = "capture"
STANDBY_AUDIO_NAME = "diagnostic_audio.wav"
STANDBY_POSE_DIRECTORY = "pose_diagnostics"
TOKEN_PATTERN = re.compile(r"[A-Za-z0-9_-]{32}")
IDENTIFIER_PATTERN = re.compile(r"[A-Za-z0-9._-]{1,128}")
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
ROLES: frozenset[str] = frozenset(("down_the_line", "face_on"))
INCIDENT_CLASSIFICATIONS: frozenset[str] = frozenset(
    (
        "successful_capture",
        "pose_armed_no_impact",
        "impact_while_not_armed",
        "audio_timestamp_mapping_rejected",
        "encoder_continuity_failure",
        "peer_failure",
        "unexpected_audio_trigger",
        "user_reported",
    )
)
FEEDBACK_CLASSIFICATIONS: frozenset[str] = frozenset(
    (
        "unreviewed",
        "good_capture",
        "armed_too_early",
        "armed_too_late",
        "missed_shot",
        "false_impact",
        "av_sync_wrong",
        "other",
    )
)
TIMING_MARK_STREAMS: Mapping[str, str] = {
    "high_speed_should_start": "preview",
    "visual_ball_impact": "high_speed",
    "audio_impact_transient": "audio",
}
CURRENT_PREVIEW_TRACE_FIELDS: frozenset[str] = frozenset(
    (
        "schema_version",
        "sequence_index",
        "timestamp_boottime_ns",
        "frame_available",
        "frame_content_type",
        "frame_byte_offset",
        "frame_byte_length",
        "model_id",
        "inference_duration_ns",
        "person_confidence",
        "address_confidence",
        "motion_magnitude",
        "hitting_region_occupied",
        "controller_state",
        "decision_reason",
    )
)
PREVIEW_CONTROLLER_STATES: frozenset[str] = frozenset(
    ("monitoring", "qualifying", "high_speed_requested", "high_speed_ready", "cooldown")
)


class HttpResponse(Protocol):
    """Minimum response surface used by the bounded HTTP reader."""

    headers: Message

    def read(self, amount: int = -1) -> bytes:
        """Read at most ``amount`` response bytes."""
        ...

    def close(self) -> None:
        """Release the response socket."""
        ...


@final
class _RejectRedirectHandler(urllib.request.HTTPRedirectHandler):
    """Keep bearer credentials on the explicitly configured phone origin."""

    # urllib fixes this six-argument callback signature; returning implicitly rejects the redirect.
    def redirect_request(  # pyright: ignore[reportIncompatibleMethodOverride, reportImplicitOverride]  # noqa: PLR0913
        self,
        request: urllib.request.Request,
        file_pointer: object,
        code: int,
        message: str,
        headers: Message,
        new_url: str,
    ) -> None:
        del request, file_pointer, code, message, headers, new_url


class CollectionError(RuntimeError):
    """An expected remote-data, transport, or local-store failure."""


@dataclasses.dataclass(frozen=True)
class NodeSpec:
    """One explicitly configured phone endpoint and its bearer credential."""

    base_url: str
    token: str

    def __post_init__(self) -> None:
        """Normalize and validate the explicit phone configuration."""
        normalized = normalize_base_url(self.base_url)
        if TOKEN_PATTERN.fullmatch(self.token) is None:
            message = "phone token must contain exactly 32 URL-safe characters"
            raise ValueError(message)
        object.__setattr__(self, "base_url", normalized)


@dataclasses.dataclass(frozen=True)
class NodeIdentity:
    """Stable identity advertised by one Android node."""

    base_url: str
    node_id: str
    role: str
    capture_profile: str

    def as_index_json(self) -> dict[str, object]:
        """Return the token-free deterministic index representation."""
        return {
            "base_url": self.base_url,
            "capture_profile": self.capture_profile,
            "node_id": self.node_id,
            "role": self.role,
        }


@dataclasses.dataclass(frozen=True)
class SessionSummary:
    """One ready session advertised by a phone."""

    session_id: str
    created_at_utc: str


@dataclasses.dataclass(frozen=True)
class HttpPayload:
    """Bounded response bytes and selected transport metadata."""

    body: bytes
    content_type: str


@dataclasses.dataclass(frozen=True)
class ArchiveInspection:
    """Validated metadata extracted without unpacking the archive to disk."""

    archive_sha256: str
    archive_bytes: int
    export_created_at_utc: str
    export_file_count: int
    manifest_created_at_utc: str
    session_kind: str
    trigger_source: str
    shared_session_id: str | None
    coordination_sha256: str | None
    linkage_status: str
    diagnostic_audio: dict[str, object] | None
    pose_diagnostics_status: str | None
    incident: dict[str, object]


@dataclasses.dataclass(frozen=True)
class CollectionResult:
    """Summary of one discovery and download pass."""

    downloaded: int
    skipped: int
    errors: tuple[str, ...]

    def as_json(self) -> dict[str, object]:
        """Return a stable CLI summary."""
        return {
            "downloaded": self.downloaded,
            "errors": list(self.errors),
            "skipped": self.skipped,
        }


Clock = Callable[[], datetime.datetime]
FailureInjector = Callable[[str], None]


@dataclasses.dataclass(frozen=True)
class PreviewExtraction:
    """Deterministic metadata for an optionally materialized preview bundle."""

    directory: PurePosixPath
    frame_count: int
    mjpeg_bytes: int
    mjpeg_sha256: str
    pose_trace_sha256: str

    def as_index_json(self) -> dict[str, object]:
        """Return the artifact-index representation."""
        return {
            "directory": self.directory.as_posix(),
            "frame_count": self.frame_count,
            "mjpeg_bytes": self.mjpeg_bytes,
            "mjpeg_sha256": self.mjpeg_sha256,
            "pose_trace_sha256": self.pose_trace_sha256,
        }


@dataclasses.dataclass(frozen=True)
class _PreviewBundleContents:
    """Validated preview files prepared before their durable publication."""

    extraction: PreviewExtraction
    mjpeg: bytes
    pose_trace: bytes
    frame_index: bytes


@dataclasses.dataclass
class _CollectionPass:
    """Mutable state shared by the small phases of one collection pass."""

    artifacts: list[dict[str, object]]
    existing_keys: set[tuple[str, str]]
    identities_by_url: dict[str, NodeIdentity]
    errors: list[str] = dataclasses.field(default_factory=list)
    nodes: list[NodeIdentity] = dataclasses.field(default_factory=list)
    collected_at: str = ""
    downloaded: int = 0
    skipped: int = 0


@dataclasses.dataclass(frozen=True)
class _PendingCollection:
    """Validated artifact inputs awaiting ordered durable publication."""

    identity: NodeIdentity
    entry: dict[str, object]
    archive: bytes
    relative_path: PurePosixPath
    preview_bundle: _PreviewBundleContents | None


@dataclasses.dataclass
class _RecoveryState:
    """Index state updated transaction-by-transaction during startup recovery."""

    artifacts: list[dict[str, object]]
    nodes: list[NodeIdentity]


def _collection_pass(index: Mapping[str, object]) -> _CollectionPass:
    artifacts = _index_artifacts(index)
    return _CollectionPass(
        artifacts=artifacts,
        existing_keys={
            (required_string(entry, "node_id"), required_string(entry, "session_id"))
            for entry in artifacts
        },
        identities_by_url={
            required_string(entry, "base_url"): _identity_from_index(entry)
            for entry in _index_nodes(index)
        },
    )


def _matching_indexed_artifact(
    artifacts: Sequence[dict[str, object]], entry: Mapping[str, object]
) -> dict[str, object] | None:
    key = (required_string(entry, "node_id"), required_string(entry, "session_id"))
    return next(
        (
            artifact
            for artifact in artifacts
            if (
                required_string(artifact, "node_id"),
                required_string(artifact, "session_id"),
            )
            == key
        ),
        None,
    )


def _validated_pending_archive(entry: Mapping[str, object], archive: Path) -> bytes:
    if archive.is_symlink() or not archive.is_file():
        raise CollectionError("pending transaction archive is not a regular file")
    expected_archive_bytes = required_nonnegative_integer(entry, "archive_bytes")
    if expected_archive_bytes > MAXIMUM_ARCHIVE_BYTES:
        raise CollectionError("pending transaction archive exceeds the configured bound")
    archive_bytes = _read_bounded_local_file(archive, expected_archive_bytes)
    if len(archive_bytes) != expected_archive_bytes:
        raise CollectionError("pending transaction archive byte count changed")
    if hashlib.sha256(archive_bytes).hexdigest() != required_sha256(entry, "archive_sha256"):
        raise CollectionError("pending transaction archive checksum changed")
    return archive_bytes


def _merge_recovered_identity(nodes: list[NodeIdentity], identity: NodeIdentity) -> None:
    previous = next((node for node in nodes if node.base_url == identity.base_url), None)
    if previous is not None and previous != identity:
        raise CollectionError("pending transaction phone identity conflicts with the index")
    if previous is None:
        candidate_nodes = [*nodes, identity]
        _validate_distinct_identities(candidate_nodes)
        nodes.append(identity)


@final
class FieldFeedbackCollector:
    """Poll configured phones and atomically retain each session archive once."""

    # The keyword-only hooks are intentional injection seams for deterministic recovery tests.
    def __init__(  # noqa: PLR0913
        self,
        nodes: Sequence[NodeSpec],
        output_directory: Path,
        *,
        timeout_seconds: float = 10.0,
        clock: Clock | None = None,
        extract_previews: bool = False,
        failure_injector: FailureInjector | None = None,
    ) -> None:
        """Configure bounded transport, storage, and optional preview extraction."""
        if len(nodes) not in (1, 2):
            message = "field feedback requires exactly one or two phones"
            raise ValueError(message)
        if len({node.base_url for node in nodes}) != len(nodes):
            message = "phone base URLs must be distinct"
            raise ValueError(message)
        if timeout_seconds <= 0:
            message = "request timeout must be positive"
            raise ValueError(message)
        self._nodes: tuple[NodeSpec, ...] = tuple(nodes)
        self._output_directory: Path = output_directory
        self._timeout_seconds: float = timeout_seconds
        self._clock: Clock = clock or (lambda: datetime.datetime.now(datetime.UTC))
        self._extract_previews: bool = extract_previews
        self._failure_injector = failure_injector
        self._http_opener = urllib.request.build_opener(_RejectRedirectHandler())

    @property
    def index_path(self) -> Path:
        """Return the collector-owned index path."""
        return self._output_directory / "index.json"

    @property
    def _transactions_directory(self) -> Path:
        return self._output_directory / ".transactions"

    def collect_once(self) -> CollectionResult:
        """Discover ready sessions, download unseen archives, and update the index."""
        self._prepare_output_directory()
        self._recover_pending_transactions()
        index = self._load_index()
        state = _collection_pass(index)
        discovered = self._discover_nodes(state)
        state.collected_at = _utc_timestamp(self._clock())
        state.nodes = sorted(
            state.identities_by_url.values(), key=lambda item: (item.node_id, item.base_url)
        )
        self._collect_discovered_sessions(discovered, state)
        if not self.index_path.exists() or _nodes_changed(index, state.nodes):
            self._write_index(state.nodes, state.artifacts, state.collected_at)
        return CollectionResult(state.downloaded, state.skipped, tuple(sorted(state.errors)))

    def _discover_nodes(
        self, state: _CollectionPass
    ) -> list[tuple[NodeSpec, NodeIdentity, list[SessionSummary]]]:
        discovered: list[tuple[NodeSpec, NodeIdentity, list[SessionSummary]]] = []
        for specification in self._nodes:
            try:
                identity = self._fetch_identity(specification)
                sessions = self._fetch_sessions(specification)
                previous_identity = state.identities_by_url.get(identity.base_url)
                if previous_identity is not None and previous_identity != identity:
                    raise CollectionError(  # noqa: TRY301
                        "phone identity changed from the retained index"
                    )
                discovered.append((specification, identity, sessions))
                state.identities_by_url[identity.base_url] = identity
            except CollectionError as error:  # noqa: PERF203
                state.errors.append(f"{specification.base_url}: {error}")

        try:
            _validate_distinct_identities([item[1] for item in discovered])
        except CollectionError as error:
            state.errors.append(str(error))
            return []
        return discovered

    def _collect_discovered_sessions(
        self,
        discovered: Sequence[tuple[NodeSpec, NodeIdentity, list[SessionSummary]]],
        state: _CollectionPass,
    ) -> None:
        for specification, identity, sessions in discovered:
            for session in sessions:
                self._collect_session(specification, identity, session, state)

    def _collect_session(
        self,
        specification: NodeSpec,
        identity: NodeIdentity,
        session: SessionSummary,
        state: _CollectionPass,
    ) -> None:
        key = (identity.node_id, session.session_id)
        if key in state.existing_keys:
            state.skipped += 1
            return
        try:
            payload = self._download_archive(specification, session.session_id)
            inspection = inspect_archive(payload.body, identity, session)
            _validate_candidate_linkage(state.artifacts, identity, session, inspection)
            relative_path = self._archive_path(identity, session, state.collected_at)
            preview_bundle = self._optional_preview_bundle(payload.body, relative_path, session)
            entry = _artifact_index_entry(
                identity, session, inspection, relative_path, state.collected_at
            )
            if preview_bundle is not None:
                entry["preview_bundle"] = preview_bundle.extraction.as_index_json()
            self._publish_collected_session(
                _PendingCollection(
                    identity=identity,
                    entry=entry,
                    archive=payload.body,
                    relative_path=relative_path,
                    preview_bundle=preview_bundle,
                ),
                state,
            )
            state.existing_keys.add(key)
            state.downloaded += 1
        except CollectionError as error:
            state.errors.append(f"{identity.node_id}/{session.session_id}: {error}")

    def _optional_preview_bundle(
        self, archive: bytes, relative_path: PurePosixPath, session: SessionSummary
    ) -> _PreviewBundleContents | None:
        if not self._extract_previews:
            return None
        return _preview_bundle_contents(archive, relative_path, session.session_id)

    def _publish_collected_session(
        self,
        pending: _PendingCollection,
        state: _CollectionPass,
    ) -> None:
        transaction_path = self._write_pending_transaction(pending.identity, pending.entry)
        self._checkpoint("transaction_durable")
        self._store_archive(pending.archive, pending.relative_path)
        self._checkpoint("archive_durable")
        if pending.preview_bundle is not None:
            _store_preview_bundle(self._output_directory, pending.preview_bundle)
        self._checkpoint("preview_durable")
        self._write_index(state.nodes, [*state.artifacts, pending.entry], state.collected_at)
        state.artifacts.append(pending.entry)
        self._checkpoint("index_durable")
        _durable_unlink(transaction_path)

    def _prepare_output_directory(self) -> None:
        if self._output_directory.is_symlink():
            raise CollectionError("output directory cannot be a symbolic link")
        _durable_mkdir(self._output_directory)
        if not self._output_directory.is_dir():
            raise CollectionError("output path is not a directory")
        artifacts = self._output_directory / "artifacts"
        if artifacts.is_symlink():
            raise CollectionError("artifact directory cannot be a symbolic link")
        _durable_mkdir(artifacts)
        transactions = self._transactions_directory
        if transactions.is_symlink():
            raise CollectionError("transaction directory cannot be a symbolic link")
        _durable_mkdir(transactions)

    def _checkpoint(self, name: str) -> None:
        if self._failure_injector is not None:
            self._failure_injector(name)

    def _write_pending_transaction(
        self,
        identity: NodeIdentity,
        entry: dict[str, object],
    ) -> Path:
        session_id = required_identifier(entry, "session_id")
        digest = hashlib.sha256(f"{identity.node_id}\0{session_id}".encode()).hexdigest()
        destination = self._transactions_directory / f"{digest}.json"
        if destination.exists() or destination.is_symlink():
            raise CollectionError("pending transaction already exists; rerun recovery first")
        transaction = {
            "schema_version": 1,
            "node": identity.as_index_json(),
            "artifact": entry,
        }
        encoded = (json.dumps(transaction, indent=2, sort_keys=True) + "\n").encode()
        _atomic_write(destination, encoded)
        return destination

    def _recover_pending_transactions(self) -> None:
        transaction_paths = _pending_transaction_paths(self._transactions_directory)
        if not transaction_paths:
            return
        index = self._load_index()
        state = _RecoveryState(
            artifacts=_index_artifacts(index),
            nodes=[_identity_from_index(value) for value in _index_nodes(index)],
        )
        for transaction_path in transaction_paths:
            self._recover_pending_transaction(transaction_path, state)

    def _recover_pending_transaction(self, transaction_path: Path, state: _RecoveryState) -> None:
        identity, entry = _read_pending_transaction(transaction_path)
        indexed = _matching_indexed_artifact(state.artifacts, entry)
        if indexed is not None:
            if indexed != entry:
                raise CollectionError("pending transaction conflicts with its indexed artifact")
            _durable_unlink(transaction_path)
            return

        archive_path = _safe_index_archive_path(required_string(entry, "archive_path"))
        archive = self._output_directory.joinpath(*archive_path.parts)
        if self._retire_unpublished_transaction(transaction_path, entry, archive_path, archive):
            return
        archive_bytes = _validated_pending_archive(entry, archive)
        session = SessionSummary(
            required_identifier(entry, "session_id"),
            required_timestamp(entry, "session_created_at_utc"),
        )
        inspection = inspect_archive(archive_bytes, identity, session)
        collected_at = required_timestamp(entry, "collected_at_utc")
        rebuilt = _artifact_index_entry(identity, session, inspection, archive_path, collected_at)
        restored_preview = self._restore_pending_preview(
            entry, archive_bytes, archive_path, session
        )
        if restored_preview is not None:
            rebuilt["preview_bundle"] = restored_preview
        if rebuilt != entry:
            raise CollectionError("pending transaction metadata contradicts its archive")
        _validate_candidate_linkage(state.artifacts, identity, session, inspection)
        _merge_recovered_identity(state.nodes, identity)
        state.artifacts.append(entry)
        state.nodes.sort(key=lambda item: (item.node_id, item.base_url))
        self._write_index(state.nodes, state.artifacts, collected_at)
        _durable_unlink(transaction_path)

    def _retire_unpublished_transaction(
        self,
        transaction_path: Path,
        entry: Mapping[str, object],
        archive_path: PurePosixPath,
        archive: Path,
    ) -> bool:
        if archive.exists():
            return False
        _cleanup_atomic_write_temps(archive)
        pending_preview = entry.get("preview_bundle")
        if pending_preview is not None:
            preview_object = require_object(pending_preview, "pending transaction preview bundle")
            preview_path = _safe_index_preview_path(
                required_string(preview_object, "directory"), archive_path
            )
            preview_directory = self._output_directory.joinpath(*preview_path.parts)
            if preview_directory.exists() or preview_directory.is_symlink():
                raise CollectionError(
                    "pending preview exists although its archive was not published"
                )
        _durable_unlink(transaction_path)
        return True

    def _restore_pending_preview(
        self,
        entry: Mapping[str, object],
        archive_bytes: bytes,
        archive_path: PurePosixPath,
        session: SessionSummary,
    ) -> dict[str, object] | None:
        expected_preview = entry.get("preview_bundle")
        if expected_preview is None:
            return None
        preview = _preview_bundle_contents(archive_bytes, archive_path, session.session_id)
        if preview is None or preview.extraction.as_index_json() != expected_preview:
            raise CollectionError("pending transaction preview metadata changed")
        _store_preview_bundle(self._output_directory, preview)
        return preview.extraction.as_index_json()

    def _load_index(self) -> dict[str, object]:
        if not self.index_path.exists():
            return {
                "schema_version": INDEX_SCHEMA_VERSION,
                "updated_at_utc": "1970-01-01T00:00:00Z",
                "nodes": [],
                "artifacts": [],
                "shared_sessions": [],
            }
        if self.index_path.is_symlink() or not self.index_path.is_file():
            raise CollectionError("index path must be a regular file")
        try:
            decoded = parse_json(self.index_path.read_bytes(), "field feedback index")
        except OSError as error:
            raise CollectionError(f"unable to read index: {error}") from error
        root = require_object(decoded, "field feedback index")
        if root.get("schema_version") != INDEX_SCHEMA_VERSION:
            raise CollectionError("unsupported field feedback index schema")
        artifacts = _index_artifacts(root)
        seen: set[tuple[str, str]] = set()
        for artifact in artifacts:
            key = (required_string(artifact, "node_id"), required_string(artifact, "session_id"))
            if key in seen:
                raise CollectionError("field feedback index contains a duplicate artifact")
            seen.add(key)
            _validate_indexed_artifact(self._output_directory, artifact)
        _index_nodes(root)
        return root

    def _fetch_identity(self, specification: NodeSpec) -> NodeIdentity:
        payload = self._request(specification, "/api/v1/node", authenticated=False)
        root = require_object(parse_json(payload.body, "node identity"), "node identity")
        if root.get("schema_version") != NODE_SCHEMA_VERSION:
            raise CollectionError("unsupported node identity schema")
        node_id = required_identifier(root, "node_id")
        role = required_string(root, "role")
        if role not in ROLES:
            raise CollectionError(f"unsupported node role: {role}")
        capture_profile = required_identifier(root, "capture_profile")
        return NodeIdentity(specification.base_url, node_id, role, capture_profile)

    def _fetch_sessions(self, specification: NodeSpec) -> list[SessionSummary]:
        payload = self._request(specification, "/api/v1/sessions", authenticated=False)
        root = require_object(parse_json(payload.body, "session list"), "session list")
        if root.get("schema_version") != SESSION_SCHEMA_VERSION:
            raise CollectionError("unsupported session list schema")
        encoded = root.get("sessions")
        if not isinstance(encoded, list):
            raise CollectionError("session list sessions must be an array")
        sessions: list[SessionSummary] = []
        seen: set[str] = set()
        for value in cast("list[object]", encoded):
            item = require_object(value, "session summary")
            session_id = required_identifier(item, "session_id")
            if session_id in seen:
                raise CollectionError("session list contains a duplicate session ID")
            seen.add(session_id)
            session_error = item.get("error")
            if not isinstance(session_error, str):
                raise CollectionError("session summary error must be a string")
            if required_string(item, "state") != "ready" or session_error:
                continue
            sessions.append(SessionSummary(session_id, required_timestamp(item, "created_at_utc")))
        return sorted(sessions, key=lambda item: (item.created_at_utc, item.session_id))

    def _download_archive(self, specification: NodeSpec, session_id: str) -> HttpPayload:
        encoded = urllib.parse.quote(session_id, safe="")
        payload = self._request(
            specification,
            f"/api/v1/sessions/{encoded}/diagnostics.zip",
            authenticated=True,
        )
        if payload.content_type.lower().partition(";")[0].strip() != "application/zip":
            raise CollectionError("diagnostic response is not application/zip")
        return payload

    def _request(self, specification: NodeSpec, path: str, *, authenticated: bool) -> HttpPayload:
        headers = {"Accept": "application/zip" if path.endswith(".zip") else "application/json"}
        if authenticated:
            headers["Authorization"] = f"Bearer {specification.token}"
        request = urllib.request.Request(  # noqa: S310
            specification.base_url + path, headers=headers
        )
        try:
            response = cast(
                "HttpResponse",
                self._http_opener.open(request, timeout=self._timeout_seconds),
            )
            try:
                body = read_bounded_response(response, MAXIMUM_ARCHIVE_BYTES)
                content_type = response.headers.get("Content-Type", "")
            finally:
                response.close()
        except urllib.error.HTTPError as error:
            raise CollectionError(f"HTTP {error.code} for {path}") from error
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            raise CollectionError(f"request failed for {path}: {error}") from error
        return HttpPayload(body, content_type)

    @staticmethod
    def _archive_path(
        identity: NodeIdentity,
        session: SessionSummary,
        collected_at: str,
    ) -> PurePosixPath:
        timestamp = _compact_timestamp(collected_at)
        return PurePosixPath(
            "artifacts",
            timestamp,
            identity.node_id,
            f"{session.session_id}-diagnostics.zip",
        )

    def _store_archive(self, body: bytes, relative: PurePosixPath) -> None:
        destination = self._output_directory.joinpath(*relative.parts)
        if destination.is_symlink():
            raise CollectionError("diagnostic destination cannot be a symbolic link")
        _atomic_write(destination, body)

    def _write_index(
        self,
        nodes: Sequence[NodeIdentity],
        artifacts: Sequence[dict[str, object]],
        updated_at: str,
    ) -> None:
        ordered_artifacts = sorted(
            artifacts,
            key=lambda entry: (
                required_string(entry, "collected_at_utc"),
                required_string(entry, "node_id"),
                required_string(entry, "session_id"),
            ),
        )
        index = {
            "schema_version": INDEX_SCHEMA_VERSION,
            "updated_at_utc": updated_at,
            "nodes": [node.as_index_json() for node in nodes],
            "artifacts": ordered_artifacts,
            "shared_sessions": _shared_session_index(ordered_artifacts),
        }
        encoded = (json.dumps(index, indent=2, sort_keys=True, ensure_ascii=False) + "\n").encode()
        _atomic_write(self.index_path, encoded)


def inspect_archive(
    body: bytes,
    identity: NodeIdentity,
    session: SessionSummary,
) -> ArchiveInspection:
    """Validate ZIP structure, declared checksums, incident, and available linkage."""
    if not body or len(body) > MAXIMUM_ARCHIVE_BYTES:
        raise CollectionError("diagnostic archive size is outside the configured bound")
    try:
        with zipfile.ZipFile(io.BytesIO(body)) as archive:
            entries = archive.infolist()
            _validate_zip_entries(entries)
            bad_entry = archive.testzip()
            if bad_entry is not None:
                raise CollectionError(f"diagnostic ZIP CRC failed for {bad_entry}")
            by_name = {entry.filename: entry for entry in entries}
            export_bytes = archive.read("diagnostic_export.json")
            export = _validate_export_manifest(export_bytes, by_name, archive, session.session_id)
            session_root = session.session_id
            manifest = require_object(
                parse_json(archive.read(f"{session_root}/manifest.json"), "session manifest"),
                "session manifest",
            )
            incident = require_object(
                parse_json(
                    archive.read(f"{session_root}/diagnostic_incident.json"),
                    "diagnostic incident",
                ),
                "diagnostic incident",
            )
            manifest_metadata = _validate_manifest(
                manifest,
                identity,
                session,
                by_name,
                archive,
            )
            incident_summary = _validate_incident(
                incident,
                identity,
                session,
                manifest_metadata,
            )
            shared_session_id = manifest_metadata["shared_session_id"]
            coordination_path = f"{session_root}/coordination_record.json"
            coordination_sha256: str | None = None
            linkage_status = "single_node"
            if coordination_path in by_name:
                coordination_bytes = archive.read(coordination_path)
                coordination = require_object(
                    parse_json(coordination_bytes, "coordination record"),
                    "coordination record",
                )
                _validate_coordination(
                    coordination,
                    identity,
                    session,
                    cast("str | None", shared_session_id),
                )
                coordination_sha256 = hashlib.sha256(coordination_bytes).hexdigest()
                linkage_status = "validated_coordination"
            elif shared_session_id is not None:
                linkage_status = "shared_session_without_coordination"
    except (KeyError, zipfile.BadZipFile, OSError) as error:
        raise CollectionError(f"invalid diagnostic ZIP: {error}") from error

    return ArchiveInspection(
        archive_sha256=hashlib.sha256(body).hexdigest(),
        archive_bytes=len(body),
        export_created_at_utc=required_timestamp(export, "created_at_utc"),
        export_file_count=len(cast("list[object]", export["files"])),
        manifest_created_at_utc=cast("str", manifest_metadata["created_at_utc"]),
        session_kind=cast("str", manifest_metadata["session_kind"]),
        trigger_source=cast("str", manifest_metadata["trigger_source"]),
        shared_session_id=cast("str | None", shared_session_id),
        coordination_sha256=coordination_sha256,
        linkage_status=linkage_status,
        diagnostic_audio=cast("dict[str, object] | None", manifest_metadata["diagnostic_audio"]),
        pose_diagnostics_status=cast("str | None", manifest_metadata["pose_diagnostics_status"]),
        incident=incident_summary,
    )


def extract_preview_bundle(
    archive_bytes: bytes,
    output_directory: Path,
    archive_path: PurePosixPath,
    session_id: str,
) -> PreviewExtraction | None:
    """Optionally materialize concatenated MJPEG/NDJSON evidence and a byte-span index."""
    contents = _preview_bundle_contents(archive_bytes, archive_path, session_id)
    if contents is None:
        return None
    _store_preview_bundle(output_directory, contents)
    return contents.extraction


def _preview_bundle_contents(
    archive_bytes: bytes,
    archive_path: PurePosixPath,
    session_id: str,
) -> _PreviewBundleContents | None:
    """Validate and prepare a preview bundle without changing the filesystem."""
    root = f"{session_id}/"
    legacy_paths = (root + PREVIEW_MJPEG_NAME, root + POSE_TRACE_NAME)
    standby_paths = (
        root + STANDBY_POSE_DIRECTORY + "/" + PREVIEW_MJPEG_NAME,
        root + STANDBY_POSE_DIRECTORY + "/" + POSE_TRACE_NAME,
    )
    try:
        with zipfile.ZipFile(io.BytesIO(archive_bytes)) as archive:
            names = set(archive.namelist())
            legacy_present = tuple(path in names for path in legacy_paths)
            standby_present = tuple(path in names for path in standby_paths)
            if legacy_present == (False, False) and standby_present == (False, False):
                return None
            if legacy_present not in ((False, False), (True, True)) or standby_present not in (
                (False, False),
                (True, True),
            ):
                raise CollectionError("preview MJPEG and pose trace must be exported together")
            if legacy_present == (True, True) and standby_present == (True, True):
                raise CollectionError("diagnostic archive contains ambiguous preview bundles")
            mjpeg_path, trace_path = (
                standby_paths if standby_present == (True, True) else legacy_paths
            )
            mjpeg = archive.read(mjpeg_path)
            trace = archive.read(trace_path)
    except (zipfile.BadZipFile, KeyError, OSError) as error:
        raise CollectionError(f"unable to read preview bundle: {error}") from error
    frame_index = _preview_frame_index(trace, mjpeg, session_id)
    bundle_relative = archive_path.with_suffix("")
    bundle_relative = bundle_relative.parent / f"{bundle_relative.name}-preview"
    decoded_index = require_object(parse_json(frame_index, "preview frame index"), "preview index")
    frames = decoded_index.get("frames")
    if not isinstance(frames, list):
        raise CollectionError("generated preview frame index is malformed")
    return _PreviewBundleContents(
        extraction=PreviewExtraction(
            directory=bundle_relative,
            frame_count=len(cast("list[object]", frames)),
            mjpeg_bytes=len(mjpeg),
            mjpeg_sha256=hashlib.sha256(mjpeg).hexdigest(),
            pose_trace_sha256=hashlib.sha256(trace).hexdigest(),
        ),
        mjpeg=mjpeg,
        pose_trace=trace,
        frame_index=frame_index,
    )


def _store_preview_bundle(output_directory: Path, contents: _PreviewBundleContents) -> None:
    bundle = output_directory.joinpath(*contents.extraction.directory.parts)
    if bundle.is_symlink():
        raise CollectionError("preview bundle destination cannot be a symbolic link")
    _durable_mkdir(bundle)
    _atomic_write(bundle / PREVIEW_MJPEG_NAME, contents.mjpeg)
    _atomic_write(bundle / POSE_TRACE_NAME, contents.pose_trace)
    _atomic_write(bundle / "frame_index.json", contents.frame_index)


def normalize_base_url(value: str) -> str:
    """Validate an explicit phone origin and remove only its trailing slash."""
    parsed = urllib.parse.urlsplit(value)
    if (
        parsed.scheme not in ("http", "https")
        or not parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
        or parsed.path not in ("", "/")
    ):
        message = "phone base URL must be a bare HTTP(S) origin"
        raise ValueError(message)
    try:
        _ = parsed.port
    except ValueError as error:
        raise ValueError("phone base URL contains an invalid port") from error
    return value.rstrip("/")


def read_bounded_response(response: HttpResponse, maximum_bytes: int) -> bytes:
    """Read a response without trusting Content-Length or accepting unbounded data."""
    declared = response.headers.get("Content-Length")
    if declared is not None:
        try:
            declared_bytes = int(declared)
        except ValueError as error:
            raise CollectionError("response Content-Length is invalid") from error
        if declared_bytes < 0 or declared_bytes > maximum_bytes:
            raise CollectionError("response exceeds the configured byte bound")
    body = response.read(maximum_bytes + 1)
    if len(body) > maximum_bytes:
        raise CollectionError("response exceeds the configured byte bound")
    return body


def parse_json(contents: bytes, label: str) -> object:
    """Decode strict UTF-8 JSON while rejecting duplicate object keys."""

    def reject_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                raise CollectionError(f"{label} contains duplicate field {key}")
            result[key] = value
        return result

    try:
        decoded = contents.decode("utf-8", errors="strict")
        return cast("object", json.loads(decoded, object_pairs_hook=reject_duplicates))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CollectionError(f"{label} is not valid UTF-8 JSON") from error


def _decode_preview_trace(trace: bytes) -> tuple[list[dict[str, object]], bool]:
    try:
        decoded_trace = trace.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise CollectionError("pose trace is not valid UTF-8") from error
    lines = decoded_trace.splitlines()
    if not lines or any(not line for line in lines):
        raise CollectionError("pose trace must contain nonempty NDJSON records")
    records = [
        require_object(
            parse_json(line.encode(), f"pose trace line {line_number + 1}"),
            "pose trace record",
        )
        for line_number, line in enumerate(lines)
    ]
    current_contract = any("frame_available" in record for record in records)
    if current_contract and any(
        set(record).symmetric_difference(CURRENT_PREVIEW_TRACE_FIELDS) for record in records
    ):
        raise CollectionError("pose trace fields do not match the current Android contract")
    return records, current_contract


def _preview_frame_index(trace: bytes, mjpeg: bytes, session_id: str) -> bytes:
    records, current_contract = _decode_preview_trace(trace)

    indexed: list[dict[str, int | str]] = []
    previous_end = 0
    previous_timestamp = -1
    for sequence_index, record in enumerate(records):
        if current_contract:
            timestamp = _validate_current_preview_trace_row(
                record, sequence_index, previous_timestamp
            )
            previous_timestamp = timestamp
            if not cast("bool", record["frame_available"]):
                continue
        offset, length = _preview_span(record)
        end = offset + length
        if offset != previous_end or length <= 0 or end > len(mjpeg):
            raise CollectionError("pose trace contains an invalid or overlapping MJPEG span")
        frame = mjpeg[offset:end]
        if not frame.startswith(b"\xff\xd8") or not frame.endswith(b"\xff\xd9"):
            raise CollectionError("pose trace span is not one complete JPEG image")
        entry: dict[str, int | str] = {
            "frame_number": len(indexed),
            "length": length,
            "offset": offset,
            "trace_line": sequence_index + 1,
        }
        if current_contract:
            entry["sequence_index"] = sequence_index
            entry["timestamp_boottime_ns"] = str(previous_timestamp)
        indexed.append(entry)
        previous_end = end
    if previous_end != len(mjpeg):
        raise CollectionError("pose trace does not index every MJPEG byte")
    document = {
        "schema_version": 1,
        "session_id": session_id,
        "mjpeg_bytes": len(mjpeg),
        "mjpeg_sha256": hashlib.sha256(mjpeg).hexdigest(),
        "observation_count": len(records),
        "pose_trace_sha256": hashlib.sha256(trace).hexdigest(),
        "frames": indexed,
    }
    if current_contract:
        document["first_timestamp_boottime_ns"] = str(
            _nonnegative_decimal(records[0], "timestamp_boottime_ns")
        )
        document["end_timestamp_boottime_ns_exclusive"] = str(previous_timestamp + 1)
    return (json.dumps(document, indent=2, sort_keys=True) + "\n").encode()


def _validate_current_preview_trace_row(
    record: Mapping[str, object], sequence_index: int, previous_timestamp: int
) -> int:
    """Validate one exact row emitted by Android ``PoseDiagnosticFiles``."""
    if (
        _nonnegative_json_integer(record.get("schema_version")) != 1
        or _nonnegative_json_integer(record.get("sequence_index")) != sequence_index
    ):
        raise CollectionError("pose trace sequence or schema version is invalid")
    timestamp = _nonnegative_decimal(record, "timestamp_boottime_ns")
    inference_duration = _nonnegative_decimal(record, "inference_duration_ns")
    if (
        timestamp > (1 << 63) - 2
        or timestamp <= previous_timestamp
        or inference_duration > (1 << 63) - 1
    ):
        raise CollectionError("pose trace timestamps are invalid or nonmonotonic")

    _validate_current_preview_frame_metadata(record)
    _bounded_preview_text(
        record.get("model_id"), MAXIMUM_PREVIEW_MODEL_ID_UTF8_BYTES, "model_id", allow_empty=False
    )
    _bounded_preview_text(
        record.get("decision_reason"),
        MAXIMUM_PREVIEW_REASON_UTF8_BYTES,
        "decision_reason",
        allow_empty=True,
    )
    _validate_current_preview_inference_values(record)
    return timestamp


def _validate_current_preview_frame_metadata(record: Mapping[str, object]) -> None:
    frame_available = record.get("frame_available")
    frame_length = record.get("frame_byte_length")
    if (
        not isinstance(frame_available, bool)
        or not isinstance(frame_length, int)
        or isinstance(frame_length, bool)
    ):
        raise CollectionError("pose trace frame availability metadata is invalid")
    if frame_available:
        if record.get("frame_content_type") != "image/jpeg" or frame_length < MINIMUM_JPEG_BYTES:
            raise CollectionError("pose trace JPEG metadata is invalid")
        _ = _nonnegative_decimal(record, "frame_byte_offset")
    elif (
        record.get("frame_content_type") is not None
        or record.get("frame_byte_offset") is not None
        or frame_length != 0
    ):
        raise CollectionError("trace-only pose observation contains JPEG metadata")


def _validate_current_preview_inference_values(record: Mapping[str, object]) -> None:
    for name in ("person_confidence", "address_confidence"):
        value = record.get(name)
        if not isinstance(value, float) or not math.isfinite(value) or value < 0.0 or value > 1.0:
            raise CollectionError(f"pose trace {name} is outside [0, 1]")
    motion = record.get("motion_magnitude")
    if not isinstance(motion, float) or not math.isfinite(motion) or motion < 0.0:
        raise CollectionError("pose trace motion_magnitude is invalid")
    if not isinstance(record.get("hitting_region_occupied"), bool):
        raise CollectionError("pose trace hitting-region state is invalid")
    if record.get("controller_state") not in PREVIEW_CONTROLLER_STATES:
        raise CollectionError("pose trace controller state is invalid")


def _bounded_preview_text(
    value: object, maximum_utf8_bytes: int, name: str, *, allow_empty: bool
) -> str:
    if not isinstance(value, str) or (not allow_empty and not value):
        raise CollectionError(f"pose trace {name} is invalid")
    try:
        encoded = value.encode("utf-8", errors="strict")
    except UnicodeEncodeError as error:
        raise CollectionError(f"pose trace {name} is not valid Unicode") from error
    if len(encoded) > maximum_utf8_bytes:
        raise CollectionError(f"pose trace {name} exceeds its byte bound")
    return value


def _preview_span(record: Mapping[str, object]) -> tuple[int, int]:
    field_pairs = (
        ("offset", "length"),
        ("byte_offset", "byte_length"),
        ("mjpeg_offset", "mjpeg_length"),
        ("mjpeg_offset_bytes", "mjpeg_length_bytes"),
        ("frame_byte_offset", "frame_byte_length"),
    )
    selected = [pair for pair in field_pairs if pair[0] in record or pair[1] in record]
    if len(selected) != 1 or not all(name in record for name in selected[0]):
        raise CollectionError("pose trace record requires one supported offset/length field pair")
    offset = _nonnegative_json_integer(record[selected[0][0]])
    length = _nonnegative_json_integer(record[selected[0][1]])
    if offset is None or length is None:
        raise CollectionError("pose trace offset and length must be nonnegative integers")
    return offset, length


def _nonnegative_json_integer(value: object) -> int | None:
    if isinstance(value, int) and not isinstance(value, bool) and value >= 0:
        return value
    if isinstance(value, str) and re.fullmatch(r"0|[1-9][0-9]*", value) is not None:
        return int(value)
    return None


def require_object(value: object, label: str) -> dict[str, object]:
    """Require a JSON object with string keys."""
    if not isinstance(value, dict):
        raise CollectionError(f"{label} must be an object")
    untyped = cast("dict[object, object]", value)
    if not all(isinstance(key, str) for key in untyped):
        raise CollectionError(f"{label} must be an object")
    return cast("dict[str, object]", untyped)


def required_string(value: Mapping[str, object], name: str) -> str:
    """Return one required nonempty JSON string."""
    result = value.get(name)
    if not isinstance(result, str) or not result:
        raise CollectionError(f"{name} must be a nonempty string")
    return result


def required_identifier(value: Mapping[str, object], name: str) -> str:
    """Return one filesystem-safe protocol identifier."""
    result = required_string(value, name)
    if IDENTIFIER_PATTERN.fullmatch(result) is None:
        raise CollectionError(f"{name} is not a safe identifier")
    return result


def required_timestamp(value: Mapping[str, object], name: str) -> str:
    """Return a normalized, timezone-aware RFC 3339 timestamp."""
    result = required_string(value, name)
    try:
        parsed = datetime.datetime.fromisoformat(result.replace("Z", "+00:00"))
    except ValueError as error:
        raise CollectionError(f"{name} must be an RFC 3339 timestamp") from error
    if parsed.tzinfo is None:
        raise CollectionError(f"{name} must include a timezone")
    return result


def required_sha256(value: Mapping[str, object], name: str) -> str:
    """Return one lowercase SHA-256 digest."""
    result = required_string(value, name)
    if SHA256_PATTERN.fullmatch(result) is None:
        raise CollectionError(f"{name} must be a lowercase SHA-256 digest")
    return result


def required_nonnegative_integer(value: Mapping[str, object], name: str) -> int:
    """Return a JSON integer while rejecting booleans and negative values."""
    result = value.get(name)
    if not isinstance(result, int) or isinstance(result, bool) or result < 0:
        raise CollectionError(f"{name} must be a nonnegative integer")
    return result


def sha256_file(path: Path) -> str:
    """Hash a retained artifact without loading it into memory."""
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(64 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise CollectionError(f"unable to hash {path}: {error}") from error
    return digest.hexdigest()


def _validate_zip_entries(entries: Sequence[zipfile.ZipInfo]) -> None:
    if len(entries) < MINIMUM_ARCHIVE_ENTRIES or len(entries) > MAXIMUM_ARCHIVE_ENTRIES:
        raise CollectionError("diagnostic ZIP entry count is outside the configured bound")
    names: set[str] = set()
    total_bytes = 0
    for entry in entries:
        if entry.filename in names:
            raise CollectionError("diagnostic ZIP contains a duplicate entry")
        names.add(entry.filename)
        if not _safe_archive_name(entry.filename) or entry.is_dir():
            raise CollectionError(f"diagnostic ZIP contains unsafe path {entry.filename!r}")
        if entry.flag_bits & 0x1:
            raise CollectionError("encrypted diagnostic ZIP entries are not accepted")
        unix_mode = entry.external_attr >> 16
        if unix_mode and stat.S_ISLNK(unix_mode):
            raise CollectionError("diagnostic ZIP symbolic links are not accepted")
        if entry.file_size < 0 or total_bytes > MAXIMUM_EXPANDED_BYTES - entry.file_size:
            raise CollectionError("diagnostic ZIP expanded size exceeds the configured bound")
        total_bytes += entry.file_size
    if "diagnostic_export.json" not in names:
        raise CollectionError("diagnostic ZIP lacks diagnostic_export.json")


def _safe_archive_name(name: str) -> bool:
    path = PurePosixPath(name)
    return (
        bool(name)
        and not name.startswith("/")
        and "\\" not in name
        and "//" not in name
        and not name.endswith("/")
        and all(
            part not in ("", ".", "..") and IDENTIFIER_PATTERN.fullmatch(part) is not None
            for part in path.parts
        )
    )


def _validate_export_manifest(  # noqa: C901, PLR0912
    contents: bytes,
    entries: Mapping[str, zipfile.ZipInfo],
    archive: zipfile.ZipFile,
    expected_session_id: str,
) -> dict[str, object]:
    export = require_object(parse_json(contents, "diagnostic export"), "diagnostic export")
    if set(export) != {"schema_version", "session_id", "created_at_utc", "files"}:
        raise CollectionError("diagnostic export fields do not match schema v1")
    if export.get("schema_version") != EXPORT_SCHEMA_VERSION:
        raise CollectionError("unsupported diagnostic export schema")
    if required_identifier(export, "session_id") != expected_session_id:
        raise CollectionError("diagnostic export belongs to another session")
    required_timestamp(export, "created_at_utc")
    encoded_files = export.get("files")
    if not isinstance(encoded_files, list) or not encoded_files:
        raise CollectionError("diagnostic export files must be a nonempty array")
    documented: set[str] = set()
    for value in cast("list[object]", encoded_files):
        item = require_object(value, "diagnostic export file")
        if set(item) != {"path", "bytes", "sha256"}:
            raise CollectionError("diagnostic export file fields do not match schema v1")
        path = required_string(item, "path")
        if path == "diagnostic_export.json" or not _safe_archive_name(path) or path in documented:
            raise CollectionError("diagnostic export contains an unsafe or duplicate path")
        documented.add(path)
        entry = entries.get(path)
        if entry is None:
            raise CollectionError(f"diagnostic export references missing file {path}")
        declared_bytes = item.get("bytes")
        if not isinstance(declared_bytes, int) or isinstance(declared_bytes, bool):
            raise CollectionError("diagnostic export byte count must be an integer")
        if declared_bytes != entry.file_size:
            raise CollectionError(f"diagnostic export byte count differs for {path}")
        if hashlib.sha256(archive.read(path)).hexdigest() != required_sha256(item, "sha256"):
            raise CollectionError(f"diagnostic export checksum differs for {path}")
    actual = set(entries).difference(("diagnostic_export.json",))
    if documented != actual:
        raise CollectionError("diagnostic export does not document every ZIP entry")
    required = {
        f"{expected_session_id}/manifest.json",
        f"{expected_session_id}/diagnostic_incident.json",
    }
    if not required.issubset(documented):
        raise CollectionError("diagnostic export lacks manifest or incident evidence")
    return export


def _validate_manifest(
    manifest: Mapping[str, object],
    identity: NodeIdentity,
    session: SessionSummary,
    entries: Mapping[str, zipfile.ZipInfo],
    archive: zipfile.ZipFile,
) -> dict[str, object]:
    if required_identifier(manifest, "session_id") != session.session_id:
        raise CollectionError("session manifest belongs to another session")
    session_kind = manifest.get("session_kind", CAPTURE_SESSION_KIND)
    if session_kind == STANDBY_SESSION_KIND:
        return _validate_standby_manifest(manifest, identity, session, entries, archive)
    if session_kind != CAPTURE_SESSION_KIND:
        raise CollectionError(f"unsupported session manifest kind: {session_kind}")
    created_at = required_timestamp(manifest, "created_at_utc")
    if created_at != session.created_at_utc:
        raise CollectionError("session-list and manifest creation times differ")
    android_capture = require_object(manifest.get("android_capture"), "android_capture")
    if required_identifier(android_capture, "node_id") != identity.node_id:
        raise CollectionError("session manifest belongs to another node")
    shared = android_capture.get("shared_session_id")
    if shared is not None and (
        not isinstance(shared, str) or IDENTIFIER_PATTERN.fullmatch(shared) is None
    ):
        raise CollectionError("manifest shared_session_id is invalid")
    views = manifest.get("views")
    if not isinstance(views, list) or len(cast("list[object]", views)) != 1:
        raise CollectionError("phone manifest must contain exactly one camera view")
    view = require_object(cast("list[object]", views)[0], "manifest camera view")
    if required_string(view, "role") != identity.role:
        raise CollectionError("manifest camera role differs from node identity")
    trigger = require_object(manifest.get("trigger"), "manifest trigger")
    pose_diagnostics_status = _validate_capture_pose_diagnostics(
        android_capture, session.session_id + "/", entries, archive, session.session_id
    )
    return {
        "created_at_utc": created_at,
        "session_kind": CAPTURE_SESSION_KIND,
        "shared_session_id": shared,
        "trigger_source": required_identifier(trigger, "source"),
        "diagnostic_audio": None,
        "pose_diagnostics_status": pose_diagnostics_status,
    }


def _validate_capture_pose_diagnostics(
    android_capture: Mapping[str, object],
    root: str,
    entries: Mapping[str, zipfile.ZipInfo],
    archive: zipfile.ZipFile,
    session_id: str,
) -> str | None:
    """Validate the current optional pose-evidence object on a retained high-speed capture."""
    value = android_capture.get("diagnostic_evidence")
    if value is None:
        return None
    evidence = require_object(value, "capture diagnostic evidence")
    if (
        set(evidence)
        != {
            "schema_version",
            "audio",
            "audio_status",
            "incident_status",
            "preview",
            "preview_status",
        }
        or _nonnegative_json_integer(evidence.get("schema_version")) != 1
    ):
        raise CollectionError("capture diagnostic evidence fields do not match schema v1")
    preview_status = required_identifier(evidence, "preview_status")
    if preview_status not in ("available", "not_available", "publication_failed"):
        raise CollectionError("capture pose preview status is unsupported")
    preview_value = evidence.get("preview")
    if preview_status == "available":
        preview = require_object(preview_value, "capture pose preview")
        _validate_capture_preview(preview, root, entries, archive, session_id)
    elif preview_value is not None:
        raise CollectionError("unavailable capture pose preview has artifact metadata")
    return preview_status


def _validate_standby_manifest(  # noqa: C901, PLR0912, PLR0915
    manifest: Mapping[str, object],
    identity: NodeIdentity,
    session: SessionSummary,
    entries: Mapping[str, zipfile.ZipInfo],
    archive: zipfile.ZipFile,
) -> dict[str, object]:
    expected_fields = {
        "schema_version",
        "session_kind",
        "session_id",
        "created_at_epoch_ms",
        "source_node_id",
        "event",
        "evidence",
        "incident",
    }
    if set(manifest) != expected_fields or manifest.get("schema_version") != 1:
        raise CollectionError("standby diagnostic manifest fields do not match schema v1")
    if required_identifier(manifest, "source_node_id") != identity.node_id:
        raise CollectionError("standby diagnostic manifest belongs to another node")
    created_at_epoch_ms = _positive_decimal(manifest, "created_at_epoch_ms")
    if created_at_epoch_ms != _timestamp_epoch_millis(session.created_at_utc):
        raise CollectionError("session-list and standby manifest creation times differ")

    event = require_object(manifest.get("event"), "standby event")
    if set(event) != {
        "sequence",
        "kind",
        "marker_audio_frame_position",
        "audio_clock_status",
        "marker_boottime_ns",
        "marker_uncertainty_ns",
        "operator_received_boottime_ns",
        "detector",
    }:
        raise CollectionError("standby event fields do not match schema v1")
    sequence = _positive_decimal(event, "sequence")
    event_kind = required_identifier(event, "kind")
    if event_kind not in ("detected_impact", "operator_tag"):
        raise CollectionError("standby event kind is unsupported")
    marker_position = _nonnegative_decimal(event, "marker_audio_frame_position")
    audio_clock_status = required_identifier(event, "audio_clock_status")
    if audio_clock_status not in ("validated", "audio_clock_unvalidated"):
        raise CollectionError("standby audio clock status is unsupported")
    marker_time = event.get("marker_boottime_ns")
    marker_uncertainty = event.get("marker_uncertainty_ns")
    marker_estimate: tuple[int, int] | None = None
    if audio_clock_status == "validated":
        marker_estimate = (
            _positive_decimal(event, "marker_boottime_ns"),
            _nonnegative_decimal(event, "marker_uncertainty_ns"),
        )
    elif marker_time is not None or marker_uncertainty is not None:
        raise CollectionError("unvalidated standby audio clock contains a marker time")
    operator_time = event.get("operator_received_boottime_ns")
    detector = event.get("detector")
    if event_kind == "operator_tag":
        if detector is not None:
            raise CollectionError("operator standby event unexpectedly contains detector evidence")
        _ = _positive_decimal(event, "operator_received_boottime_ns")
    elif operator_time is not None or not isinstance(detector, dict):
        raise CollectionError("detected standby event lacks detector evidence")
    else:
        _validate_standby_detector(
            cast("Mapping[str, object]", detector),
            marker_position,
            marker_estimate,
        )

    evidence = require_object(manifest.get("evidence"), "standby evidence")
    if set(evidence) != {"audio", "preview_status", "preview"}:
        raise CollectionError("standby evidence fields do not match schema v1")
    audio = require_object(evidence.get("audio"), "standby audio evidence")
    if set(audio) != {
        "path",
        "content_type",
        "bytes",
        "sample_rate_hz",
        "first_frame_position",
        "end_frame_position",
        "marker_frame_position",
        "sample_count",
        "marker_sample_index",
    }:
        raise CollectionError("standby audio evidence fields do not match schema v1")
    audio_path = required_string(audio, "path")
    if audio_path != STANDBY_AUDIO_NAME or required_string(audio, "content_type") != "audio/wav":
        raise CollectionError("standby audio evidence path or content type is unsupported")
    audio_bytes = _positive_decimal(audio, "bytes")
    sample_rate_hz = audio.get("sample_rate_hz")
    sample_count = audio.get("sample_count")
    marker_sample_index = audio.get("marker_sample_index")
    if (
        not isinstance(sample_rate_hz, int)
        or isinstance(sample_rate_hz, bool)
        or sample_rate_hz != STANDBY_AUDIO_SAMPLE_RATE_HZ
        or not isinstance(sample_count, int)
        or isinstance(sample_count, bool)
        or sample_count <= 0
        or not isinstance(marker_sample_index, int)
        or isinstance(marker_sample_index, bool)
        or marker_sample_index < 0
        or marker_sample_index >= sample_count
        or audio_bytes != 44 + sample_count * 2
    ):
        raise CollectionError("standby audio evidence metadata is invalid")
    first_position = _nonnegative_decimal(audio, "first_frame_position")
    end_position = _positive_decimal(audio, "end_frame_position")
    audio_marker = _nonnegative_decimal(audio, "marker_frame_position")
    if (
        end_position - first_position != sample_count
        or audio_marker - first_position != marker_sample_index
        or audio_marker != marker_position
    ):
        raise CollectionError("standby audio frame positions contradict the event")
    root = session.session_id + "/"
    audio_archive_path = root + audio_path
    audio_entry = entries.get(audio_archive_path)
    if audio_entry is None or audio_entry.file_size != audio_bytes:
        raise CollectionError("standby diagnostic archive lacks its declared WAV")
    _validate_pcm16_wav(archive.read(audio_archive_path), sample_rate_hz, sample_count)

    incident = require_object(manifest.get("incident"), "standby incident reference")
    if set(incident) != {"path", "bytes"}:
        raise CollectionError("standby incident reference fields do not match schema v1")
    incident_path = required_string(incident, "path")
    incident_bytes = incident.get("bytes")
    incident_entry = entries.get(root + incident_path)
    if (
        incident_path != "diagnostic_incident.json"
        or not isinstance(incident_bytes, int)
        or isinstance(incident_bytes, bool)
        or incident_bytes <= 0
        or incident_entry is None
        or incident_entry.file_size != incident_bytes
    ):
        raise CollectionError("standby incident reference is invalid")

    preview_status = required_identifier(evidence, "preview_status")
    if preview_status not in ("available", "not_available", "publication_failed"):
        raise CollectionError("standby preview status is unsupported")
    preview_value = evidence.get("preview")
    if preview_status == "available":
        preview = require_object(preview_value, "standby pose preview")
        _validate_standby_preview(preview, root, entries, archive, session.session_id)
    elif preview_value is not None:
        raise CollectionError("unavailable standby preview unexpectedly has artifact metadata")

    return {
        "created_at_utc": session.created_at_utc,
        "session_kind": STANDBY_SESSION_KIND,
        "shared_session_id": None,
        "trigger_source": event_kind,
        "diagnostic_audio": {
            "path": audio_path,
            "bytes": audio_bytes,
            "sample_rate_hz": sample_rate_hz,
            "sample_count": sample_count,
            "marker_sample_index": marker_sample_index,
        },
        "pose_diagnostics_status": preview_status,
        "event_sequence": sequence,
        "created_at_epoch_ms": created_at_epoch_ms,
        "audio_minimum_offset_us": _ceiling_divide(
            (first_position - audio_marker) * 1_000_000,
            sample_rate_hz,
        ),
        "audio_maximum_offset_us": _ceiling_divide(
            (end_position - audio_marker) * 1_000_000,
            sample_rate_hz,
        )
        - 1,
    }


def _validate_standby_preview(
    preview: Mapping[str, object],
    root: str,
    entries: Mapping[str, zipfile.ZipInfo],
    archive: zipfile.ZipFile,
    session_id: str,
) -> None:
    if set(preview) != {
        "frames_path",
        "frames_content_type",
        "frames_bytes",
        "trace_path",
        "trace_content_type",
        "trace_bytes",
        "first_timestamp_boottime_ns",
        "end_timestamp_boottime_ns_exclusive",
        "observation_count",
        "jpeg_frame_count",
        "frame_count",
    }:
        raise CollectionError("standby pose preview fields do not match schema v1")
    _ = _validate_pose_preview_artifacts(preview, root, entries, archive, session_id)


def _validate_capture_preview(
    preview: Mapping[str, object],
    root: str,
    entries: Mapping[str, zipfile.ZipInfo],
    archive: zipfile.ZipFile,
    session_id: str,
) -> None:
    if (
        set(preview)
        != {
            "schema_version",
            "frames_path",
            "frames_content_type",
            "frames_bytes",
            "trace_path",
            "trace_content_type",
            "trace_bytes",
            "first_timestamp_boottime_ns",
            "end_timestamp_boottime_ns_exclusive",
            "observation_count",
            "jpeg_frame_count",
            "frame_count",
            "frame_index",
        }
        or _nonnegative_json_integer(preview.get("schema_version")) != 1
    ):
        raise CollectionError("capture pose preview fields do not match schema v1")
    generated = _validate_pose_preview_artifacts(preview, root, entries, archive, session_id)
    encoded_index = preview.get("frame_index")
    generated_index = generated.get("frames")
    if not isinstance(encoded_index, list) or not isinstance(generated_index, list):
        raise CollectionError("capture pose preview frame index is invalid")
    decoded_generated = [
        require_object(value, "generated pose frame")
        for value in cast("list[object]", generated_index)
    ]
    decoded_encoded = [
        require_object(value, "capture pose frame") for value in cast("list[object]", encoded_index)
    ]
    if len(decoded_encoded) != len(decoded_generated):
        raise CollectionError("capture pose preview frame index count is inconsistent")
    for expected, actual in zip(decoded_generated, decoded_encoded, strict=True):
        if set(actual) != {
            "sequence_index",
            "timestamp_boottime_ns",
            "byte_offset",
            "byte_length",
            "content_type",
        }:
            raise CollectionError("capture pose preview frame-index fields are invalid")
        if (
            _nonnegative_json_integer(actual.get("sequence_index"))
            != expected.get("sequence_index")
            or _nonnegative_decimal(actual, "timestamp_boottime_ns")
            != int(required_string(expected, "timestamp_boottime_ns"))
            or _nonnegative_decimal(actual, "byte_offset") != expected.get("offset")
            or _nonnegative_json_integer(actual.get("byte_length")) != expected.get("length")
            or actual.get("content_type") != "image/jpeg"
        ):
            raise CollectionError("capture pose preview frame index contradicts its trace")


def _validate_pose_preview_artifacts(
    preview: Mapping[str, object],
    root: str,
    entries: Mapping[str, zipfile.ZipInfo],
    archive: zipfile.ZipFile,
    session_id: str,
) -> dict[str, object]:
    frames_path = required_string(preview, "frames_path")
    trace_path = required_string(preview, "trace_path")
    if (
        frames_path != f"{STANDBY_POSE_DIRECTORY}/{PREVIEW_MJPEG_NAME}"
        or trace_path != f"{STANDBY_POSE_DIRECTORY}/{POSE_TRACE_NAME}"
        or required_string(preview, "frames_content_type") != "image/jpeg"
        or required_string(preview, "trace_content_type") != "application/x-ndjson"
    ):
        raise CollectionError("pose preview paths or content types are unsupported")
    frames_bytes = _nonnegative_decimal(preview, "frames_bytes")
    trace_bytes = preview.get("trace_bytes")
    observation_count = preview.get("observation_count")
    jpeg_frame_count = preview.get("jpeg_frame_count")
    frame_count = preview.get("frame_count")
    first_timestamp = _nonnegative_decimal(preview, "first_timestamp_boottime_ns")
    end_timestamp = _positive_decimal(preview, "end_timestamp_boottime_ns_exclusive")
    if (
        frames_bytes > 32 * 1024 * 1024
        or not isinstance(trace_bytes, int)
        or isinstance(trace_bytes, bool)
        or trace_bytes <= 0
        or trace_bytes > 1024 * 1024
        or not isinstance(observation_count, int)
        or isinstance(observation_count, bool)
        or observation_count <= 0
        or observation_count > MAXIMUM_PREVIEW_OBSERVATIONS
        or not isinstance(jpeg_frame_count, int)
        or isinstance(jpeg_frame_count, bool)
        or jpeg_frame_count < 0
        or jpeg_frame_count > observation_count
        or not isinstance(frame_count, int)
        or isinstance(frame_count, bool)
        or frame_count != jpeg_frame_count
        or ((frames_bytes == 0) != (jpeg_frame_count == 0))
        or end_timestamp <= first_timestamp
    ):
        raise CollectionError("pose preview metadata is invalid")
    frames_entry = entries.get(root + frames_path)
    trace_entry = entries.get(root + trace_path)
    if (
        frames_entry is None
        or frames_entry.file_size != frames_bytes
        or trace_entry is None
        or trace_entry.file_size != trace_bytes
    ):
        raise CollectionError("pose preview files disagree with their manifest")
    generated = require_object(
        parse_json(
            _preview_frame_index(
                archive.read(root + trace_path),
                archive.read(root + frames_path),
                session_id,
            ),
            "pose preview index",
        ),
        "pose preview index",
    )
    indexed_frames = generated.get("frames")
    if not isinstance(indexed_frames, list):
        raise CollectionError("pose preview index has no frame list")
    if (
        len(cast("list[object]", indexed_frames)) != jpeg_frame_count
        or generated.get("observation_count") != observation_count
        or generated.get("first_timestamp_boottime_ns") != str(first_timestamp)
        or generated.get("end_timestamp_boottime_ns_exclusive") != str(end_timestamp)
    ):
        raise CollectionError("pose preview counts or timestamps contradict its trace")
    return generated


def _validate_standby_detector(
    detector: Mapping[str, object],
    marker_position: int,
    marker_estimate: tuple[int, int] | None,
) -> None:
    expected_fields = {
        "strike_frame_position",
        "confirmation_frame_position",
        "peak_amplitude",
        "noise_floor",
        "threshold",
        "strike_boottime_ns",
        "strike_uncertainty_ns",
        "confirmation_boottime_ns",
        "confirmation_uncertainty_ns",
    }
    if set(detector) != expected_fields:
        raise CollectionError("standby detector evidence fields do not match schema v1")
    strike_position = _nonnegative_decimal(detector, "strike_frame_position")
    confirmation_position = _nonnegative_decimal(detector, "confirmation_frame_position")
    if strike_position != marker_position or confirmation_position < strike_position:
        raise CollectionError("standby detector frame positions contradict the event")
    for field in ("peak_amplitude", "noise_floor", "threshold"):
        encoded = required_string(detector, field)
        if re.fullmatch(r"(?:0\.[0-9]+|1\.0|[1-9]\.[0-9]+E-[1-9][0-9]*)", encoded) is None:
            raise CollectionError(f"standby detector {field} is not a canonical float")
        parsed = float(encoded)
        if not math.isfinite(parsed) or parsed < 0.0 or parsed > 1.0:
            raise CollectionError(f"standby detector {field} is outside [0, 1]")
    strike_estimate = _optional_standby_estimate(detector, "strike")
    _ = _optional_standby_estimate(detector, "confirmation")
    if strike_estimate != marker_estimate:
        raise CollectionError("standby detector strike time contradicts the event marker")


def _optional_standby_estimate(value: Mapping[str, object], prefix: str) -> tuple[int, int] | None:
    boottime = value.get(f"{prefix}_boottime_ns")
    uncertainty = value.get(f"{prefix}_uncertainty_ns")
    if boottime is None and uncertainty is None:
        return None
    if boottime is None or uncertainty is None:
        raise CollectionError(f"standby detector {prefix} time is incomplete")
    return (
        _positive_decimal(value, f"{prefix}_boottime_ns"),
        _nonnegative_decimal(value, f"{prefix}_uncertainty_ns"),
    )


def _ceiling_divide(numerator: int, positive_denominator: int) -> int:
    return -(-numerator // positive_denominator)


def _positive_decimal(value: Mapping[str, object], name: str) -> int:
    parsed = _nonnegative_decimal(value, name)
    if parsed <= 0:
        raise CollectionError(f"{name} must be a positive decimal string")
    return parsed


def _nonnegative_decimal(value: Mapping[str, object], name: str) -> int:
    encoded = value.get(name)
    if not isinstance(encoded, str) or re.fullmatch(r"0|[1-9][0-9]*", encoded) is None:
        raise CollectionError(f"{name} must be a nonnegative decimal string")
    return int(encoded)


def _timestamp_epoch_millis(timestamp: str) -> int:
    try:
        parsed = datetime.datetime.fromisoformat(timestamp.replace("Z", "+00:00"))
    except ValueError as error:
        raise CollectionError("session created_at_utc must be an RFC 3339 timestamp") from error
    if parsed.tzinfo is None:
        raise CollectionError("session created_at_utc must include a timezone")
    return int(parsed.timestamp() * 1_000)


def _validate_pcm16_wav(contents: bytes, sample_rate_hz: int, sample_count: int) -> None:
    if (
        len(contents) != 44 + sample_count * 2
        or contents[0:4] != b"RIFF"
        or int.from_bytes(contents[4:8], "little") != len(contents) - 8
        or contents[8:12] != b"WAVE"
        or contents[12:16] != b"fmt "
        or int.from_bytes(contents[16:20], "little") != PCM16_WAVE_FORMAT_CHUNK_BYTES
        or int.from_bytes(contents[20:22], "little") != 1
        or int.from_bytes(contents[22:24], "little") != 1
        or int.from_bytes(contents[24:28], "little") != sample_rate_hz
        or int.from_bytes(contents[28:32], "little") != sample_rate_hz * 2
        or int.from_bytes(contents[32:34], "little") != PCM16_MONO_BLOCK_ALIGN_BYTES
        or int.from_bytes(contents[34:36], "little") != PCM16_BITS_PER_SAMPLE
        or contents[36:40] != b"data"
        or int.from_bytes(contents[40:44], "little") != sample_count * 2
    ):
        raise CollectionError("standby diagnostic WAV is not canonical mono PCM16")


def _validate_incident(
    incident: Mapping[str, object],
    identity: NodeIdentity,
    session: SessionSummary,
    manifest_metadata: Mapping[str, object],
) -> dict[str, object]:
    expected_fields = {
        "schema_version",
        "incident_id",
        "classification",
        "created_at_epoch_ms",
        "source_node_id",
        "user_feedback",
        "timing_marks",
    }
    if (
        set(incident) != expected_fields
        or incident.get("schema_version") != INCIDENT_SCHEMA_VERSION
    ):
        raise CollectionError("diagnostic incident fields do not match schema v1")
    if required_identifier(incident, "incident_id") != session.session_id:
        raise CollectionError("diagnostic incident belongs to another session")
    if required_identifier(incident, "source_node_id") != identity.node_id:
        raise CollectionError("diagnostic incident belongs to another node")
    classification = required_identifier(incident, "classification")
    if classification not in INCIDENT_CLASSIFICATIONS:
        raise CollectionError("diagnostic incident classification is unsupported")
    created_at_epoch_ms = _positive_decimal(incident, "created_at_epoch_ms")
    feedback_classification, note = _validate_incident_feedback(incident.get("user_feedback"))
    parsed_marks = _validate_incident_timing_marks(incident.get("timing_marks"))
    if manifest_metadata.get("session_kind") == STANDBY_SESSION_KIND:
        _validate_standby_incident_semantics(
            classification,
            created_at_epoch_ms,
            feedback_classification,
            parsed_marks,
            manifest_metadata,
        )
    return {
        "classification": classification,
        "created_at_epoch_ms": str(created_at_epoch_ms),
        "feedback_classification": feedback_classification,
        "feedback_note": note,
        "timing_mark_count": len(parsed_marks),
    }


def _validate_incident_feedback(value: object) -> tuple[str, str]:
    feedback = require_object(value, "diagnostic user feedback")
    if set(feedback) != {"classification", "note"}:
        raise CollectionError("diagnostic user feedback fields do not match schema v1")
    feedback_classification = required_identifier(feedback, "classification")
    if feedback_classification not in FEEDBACK_CLASSIFICATIONS:
        raise CollectionError("diagnostic feedback classification is unsupported")
    note = feedback.get("note")
    if (
        not isinstance(note, str)
        or len(note) > MAXIMUM_FEEDBACK_NOTE_LENGTH
        or len(note.encode()) > MAXIMUM_FEEDBACK_NOTE_UTF8_BYTES
        or (feedback_classification == "unreviewed" and note)
    ):
        raise CollectionError("diagnostic feedback note is invalid")
    return feedback_classification, note


def _validate_incident_timing_marks(value: object) -> list[tuple[int, str, str]]:
    timing_marks = value
    if (
        not isinstance(timing_marks, list)
        or len(cast("list[object]", timing_marks)) > MAXIMUM_TIMING_MARKS
    ):
        raise CollectionError("diagnostic timing marks are invalid")
    parsed_marks = [
        _validate_incident_timing_mark(value) for value in cast("list[object]", timing_marks)
    ]
    identities = [(mark[1], mark[2]) for mark in parsed_marks]
    if len(set(identities)) != len(identities):
        raise CollectionError("diagnostic timing marks contain a duplicate kind and stream")
    if parsed_marks != sorted(parsed_marks):
        raise CollectionError("diagnostic timing marks are not in canonical order")
    return parsed_marks


def _validate_incident_timing_mark(value: object) -> tuple[int, str, str]:
    mark = require_object(value, "diagnostic timing mark")
    if set(mark) != {"kind", "stream_id", "offset_us"}:
        raise CollectionError("diagnostic timing mark fields do not match schema v1")
    kind = required_identifier(mark, "kind")
    expected_stream = TIMING_MARK_STREAMS.get(kind)
    if expected_stream is None:
        raise CollectionError("diagnostic timing mark kind is unsupported")
    stream = required_identifier(mark, "stream_id")
    if stream != expected_stream and not (
        kind == "audio_impact_transient" and stream == "standby_audio"
    ):
        raise CollectionError("diagnostic timing mark uses the wrong stream")
    offset = _signed_decimal(mark, "offset_us")
    return (offset, kind, stream)


def _validate_standby_incident_semantics(
    classification: str,
    created_at_epoch_ms: int,
    feedback_classification: str,
    timing_marks: Sequence[tuple[int, str, str]],
    manifest_metadata: Mapping[str, object],
) -> None:
    event_kind = cast("str", manifest_metadata["trigger_source"])
    expected_classification = (
        "impact_while_not_armed" if event_kind == "detected_impact" else "user_reported"
    )
    if classification != expected_classification:
        raise CollectionError("standby incident classification contradicts its event")
    if created_at_epoch_ms != cast("int", manifest_metadata["created_at_epoch_ms"]):
        raise CollectionError("standby incident creation time contradicts its manifest")
    if event_kind == "operator_tag" and feedback_classification == "unreviewed":
        raise CollectionError("operator standby incident cannot remain unreviewed")
    minimum_audio_us = cast("int", manifest_metadata["audio_minimum_offset_us"])
    maximum_audio_us = cast("int", manifest_metadata["audio_maximum_offset_us"])
    for offset, kind, stream in timing_marks:
        if kind != "audio_impact_transient":
            raise CollectionError("standby incident contains a video timing mark")
        if offset < minimum_audio_us or offset > maximum_audio_us:
            raise CollectionError("standby incident audio timing mark is outside retained evidence")
        if stream == "standby_audio" and (
            event_kind != "detected_impact"
            or offset != 0
            or feedback_classification != "unreviewed"
        ):
            raise CollectionError("standby detector timing mark contradicts its incident state")
    if (
        event_kind == "detected_impact"
        and feedback_classification == "unreviewed"
        and list(timing_marks) != [(0, "audio_impact_transient", "standby_audio")]
    ):
        raise CollectionError("unreviewed standby impact lacks its canonical detector timing mark")


def _signed_decimal(value: Mapping[str, object], name: str) -> int:
    encoded = value.get(name)
    if not isinstance(encoded, str) or re.fullmatch(r"0|-?[1-9][0-9]*", encoded) is None:
        raise CollectionError(f"{name} must be a canonical signed decimal string")
    parsed = int(encoded)
    if parsed < -(1 << 63) or parsed > (1 << 63) - 1:
        raise CollectionError(f"{name} exceeds signed 64-bit range")
    return parsed


def _validate_coordination(
    record: Mapping[str, object],
    identity: NodeIdentity,
    session: SessionSummary,
    shared_session_id: str | None,
) -> None:
    if shared_session_id is None:
        raise CollectionError("coordination evidence exists without a shared session")
    if (
        record.get("schema_version") != 1
        or required_identifier(record, "shared_session_id") != shared_session_id
    ):
        raise CollectionError("coordination record belongs to another shared session")
    matches = False
    for role in ("down_the_line", "face_on"):
        evidence = require_object(record.get(role), f"coordination {role}")
        if required_string(evidence, "role") != role:
            raise CollectionError("coordination evidence role is inconsistent")
        if (
            required_identifier(evidence, "node_id") == identity.node_id
            and required_identifier(evidence, "local_session_id") == session.session_id
        ):
            matches = True
    if not matches:
        raise CollectionError("coordination record does not contain the local session")


def _validate_candidate_linkage(
    existing: Sequence[dict[str, object]],
    identity: NodeIdentity,
    session: SessionSummary,
    inspection: ArchiveInspection,
) -> None:
    shared = inspection.shared_session_id
    if shared is None:
        return
    for artifact in existing:
        if artifact.get("shared_session_id") != shared:
            continue
        if required_string(artifact, "node_id") == identity.node_id:
            raise CollectionError("shared session contains multiple local sessions from one node")
        if required_string(artifact, "node_role") == identity.role:
            raise CollectionError("shared session contains duplicate camera roles")
        existing_coordination = artifact.get("coordination_sha256")
        if (
            isinstance(existing_coordination, str)
            and inspection.coordination_sha256 is not None
            and existing_coordination != inspection.coordination_sha256
        ):
            raise CollectionError("paired nodes supplied conflicting coordination evidence")
        if required_string(artifact, "session_id") == session.session_id:
            raise CollectionError("different nodes reused one local session ID")


def _artifact_index_entry(
    identity: NodeIdentity,
    session: SessionSummary,
    inspection: ArchiveInspection,
    archive_path: PurePosixPath,
    collected_at: str,
) -> dict[str, object]:
    return {
        "archive_bytes": inspection.archive_bytes,
        "archive_path": archive_path.as_posix(),
        "archive_sha256": inspection.archive_sha256,
        "checksum_validation": "passed",
        "collected_at_utc": collected_at,
        "coordination_sha256": inspection.coordination_sha256,
        "diagnostic_audio": inspection.diagnostic_audio,
        "diagnostic_incident": inspection.incident,
        "export_created_at_utc": inspection.export_created_at_utc,
        "export_file_count": inspection.export_file_count,
        "linkage_status": inspection.linkage_status,
        "manifest_created_at_utc": inspection.manifest_created_at_utc,
        "node_id": identity.node_id,
        "node_role": identity.role,
        "session_created_at_utc": session.created_at_utc,
        "session_id": session.session_id,
        "session_kind": inspection.session_kind,
        "shared_session_id": inspection.shared_session_id,
        "pose_diagnostics_status": inspection.pose_diagnostics_status,
        "trigger_source": inspection.trigger_source,
    }


def _shared_session_index(artifacts: Sequence[dict[str, object]]) -> list[dict[str, object]]:
    grouped: dict[str, list[dict[str, object]]] = {}
    for artifact in artifacts:
        shared = artifact.get("shared_session_id")
        if isinstance(shared, str):
            grouped.setdefault(shared, []).append(artifact)
    result: list[dict[str, object]] = []
    for shared, members in sorted(grouped.items()):
        ordered = sorted(
            members,
            key=lambda item: (required_string(item, "node_role"), required_string(item, "node_id")),
        )
        roles = [required_string(item, "node_role") for item in ordered]
        result.append(
            {
                "artifacts": [
                    {
                        "node_id": required_string(item, "node_id"),
                        "role": required_string(item, "node_role"),
                        "session_id": required_string(item, "session_id"),
                    }
                    for item in ordered
                ],
                "shared_session_id": shared,
                "status": (
                    "paired"
                    if frozenset(roles) == ROLES and len(roles) == DUAL_NODE_COUNT
                    else "partial"
                ),
            }
        )
    return result


def _index_artifacts(index: Mapping[str, object]) -> list[dict[str, object]]:
    value = index.get("artifacts")
    if not isinstance(value, list):
        raise CollectionError("field feedback index artifacts must be an array")
    return [require_object(item, "indexed artifact") for item in cast("list[object]", value)]


def _index_nodes(index: Mapping[str, object]) -> list[dict[str, object]]:
    value = index.get("nodes")
    if not isinstance(value, list):
        raise CollectionError("field feedback index nodes must be an array")
    return [require_object(item, "indexed node") for item in cast("list[object]", value)]


def _identity_from_index(value: Mapping[str, object]) -> NodeIdentity:
    return NodeIdentity(
        required_string(value, "base_url"),
        required_identifier(value, "node_id"),
        required_string(value, "role"),
        required_identifier(value, "capture_profile"),
    )


def _nodes_changed(index: Mapping[str, object], nodes: Sequence[NodeIdentity]) -> bool:
    return _index_nodes(index) != [node.as_index_json() for node in nodes]


def _validate_distinct_identities(identities: Iterable[NodeIdentity]) -> None:
    values = list(identities)
    if len({value.node_id for value in values}) != len(values):
        raise CollectionError("configured phone endpoints advertise the same node ID")
    if len(values) == DUAL_NODE_COUNT and len({value.role for value in values}) != DUAL_NODE_COUNT:
        raise CollectionError("two configured phones must advertise distinct camera roles")


def _safe_index_archive_path(value: str) -> PurePosixPath:
    path = PurePosixPath(value)
    if (
        path.is_absolute()
        or not value.startswith("artifacts/")
        or not _safe_archive_name(value)
        or path.suffix != ".zip"
    ):
        raise CollectionError("indexed archive path is unsafe")
    return path


def _safe_index_preview_path(value: str, archive_path: PurePosixPath) -> PurePosixPath:
    path = PurePosixPath(value)
    archive_without_suffix = archive_path.with_suffix("")
    expected = archive_without_suffix.parent / f"{archive_without_suffix.name}-preview"
    if (
        path.is_absolute()
        or not value.startswith("artifacts/")
        or not _safe_archive_name(value)
        or path != expected
    ):
        raise CollectionError("indexed preview path is unsafe or does not match its archive")
    return path


def _validate_preview_index_entry(
    output_directory: Path,
    artifact: Mapping[str, object],
    archive_path: PurePosixPath,
) -> None:
    value = artifact.get("preview_bundle")
    if value is None:
        return
    preview = require_object(value, "indexed preview bundle")
    if set(preview) != {
        "directory",
        "frame_count",
        "mjpeg_bytes",
        "mjpeg_sha256",
        "pose_trace_sha256",
    }:
        raise CollectionError("indexed preview bundle fields do not match schema v1")
    mjpeg, trace, frame_index = _read_indexed_preview_bundle(
        output_directory, preview, archive_path
    )
    if hashlib.sha256(mjpeg).hexdigest() != required_sha256(preview, "mjpeg_sha256"):
        raise CollectionError("indexed preview MJPEG checksum changed")
    if hashlib.sha256(trace).hexdigest() != required_sha256(preview, "pose_trace_sha256"):
        raise CollectionError("indexed preview pose trace checksum changed")
    session_id = required_identifier(artifact, "session_id")
    expected_frame_index = _preview_frame_index(trace, mjpeg, session_id)
    if frame_index != expected_frame_index:
        raise CollectionError("indexed preview frame index changed")
    decoded = require_object(parse_json(frame_index, "indexed preview frame index"), "frame index")
    frames = decoded.get("frames")
    if not isinstance(frames, list) or len(
        cast("list[object]", frames)
    ) != required_nonnegative_integer(preview, "frame_count"):
        raise CollectionError("indexed preview frame count changed")


def _read_indexed_preview_bundle(
    output_directory: Path,
    preview: Mapping[str, object],
    archive_path: PurePosixPath,
) -> tuple[bytes, bytes, bytes]:
    preview_path = _safe_index_preview_path(required_string(preview, "directory"), archive_path)
    bundle = output_directory.joinpath(*preview_path.parts)
    if bundle.is_symlink() or not bundle.is_dir():
        raise CollectionError(f"indexed preview bundle is missing: {preview_path.as_posix()}")
    mjpeg_path = bundle / PREVIEW_MJPEG_NAME
    trace_path = bundle / POSE_TRACE_NAME
    frame_index_path = bundle / "frame_index.json"
    for path in (mjpeg_path, trace_path, frame_index_path):
        if path.is_symlink() or not path.is_file():
            raise CollectionError(f"indexed preview file is missing: {path.name}")
    expected_mjpeg_bytes = required_nonnegative_integer(preview, "mjpeg_bytes")
    if expected_mjpeg_bytes > MAXIMUM_EXPANDED_BYTES:
        raise CollectionError("indexed preview MJPEG exceeds the configured bound")
    mjpeg = _read_bounded_local_file(mjpeg_path, expected_mjpeg_bytes)
    trace = _read_bounded_local_file(trace_path, MAXIMUM_EXPANDED_BYTES)
    frame_index = _read_bounded_local_file(frame_index_path, MAXIMUM_EXPANDED_BYTES)
    if len(mjpeg) != expected_mjpeg_bytes:
        raise CollectionError("indexed preview MJPEG byte count changed")
    return mjpeg, trace, frame_index


def _validate_indexed_artifact(
    output_directory: Path,
    artifact: Mapping[str, object],
) -> None:
    archive_path = _safe_index_archive_path(required_string(artifact, "archive_path"))
    absolute_path = output_directory.joinpath(*archive_path.parts)
    if absolute_path.is_symlink() or not absolute_path.is_file():
        raise CollectionError(f"indexed archive is missing: {archive_path.as_posix()}")
    try:
        archive_size = absolute_path.stat().st_size
    except OSError as error:
        raise CollectionError(f"unable to stat indexed archive: {error}") from error
    expected_archive_bytes = required_nonnegative_integer(artifact, "archive_bytes")
    if expected_archive_bytes > MAXIMUM_ARCHIVE_BYTES:
        raise CollectionError("indexed archive exceeds the configured bound")
    if archive_size != expected_archive_bytes:
        raise CollectionError(
            f"indexed archive checksum changed (byte count): {archive_path.as_posix()}"
        )
    expected = required_sha256(artifact, "archive_sha256")
    if sha256_file(absolute_path) != expected:
        raise CollectionError(f"indexed archive checksum changed: {archive_path.as_posix()}")
    _validate_preview_index_entry(output_directory, artifact, archive_path)


def _pending_transaction_paths(directory: Path) -> list[Path]:
    try:
        entries = sorted(directory.iterdir(), key=lambda path: path.name)
    except OSError as error:
        raise CollectionError(f"unable to inspect pending transactions: {error}") from error
    if len(entries) > MAXIMUM_PENDING_TRANSACTIONS * 2:
        raise CollectionError("pending transaction directory entry count exceeds the bound")
    transactions: list[Path] = []
    for path in entries:
        if path.is_symlink() or not path.is_file():
            raise CollectionError("pending transaction directory contains a non-regular entry")
        if path.name.startswith(".") and path.name.endswith(".tmp"):
            _durable_unlink(path)
            continue
        if re.fullmatch(r"[0-9a-f]{64}\.json", path.name) is None:
            raise CollectionError("pending transaction directory contains an unknown file")
        transactions.append(path)
        if len(transactions) > MAXIMUM_PENDING_TRANSACTIONS:
            raise CollectionError("pending transaction count exceeds the configured bound")
    return transactions


def _read_pending_transaction(path: Path) -> tuple[NodeIdentity, dict[str, object]]:
    contents = _read_bounded_local_file(path, MAXIMUM_TRANSACTION_BYTES)
    root = require_object(parse_json(contents, "pending transaction"), "transaction")
    if set(root) != {"schema_version", "node", "artifact"} or root.get("schema_version") != 1:
        raise CollectionError("pending transaction fields do not match schema v1")
    node = require_object(root.get("node"), "pending transaction node")
    if set(node) != {"base_url", "capture_profile", "node_id", "role"}:
        raise CollectionError("pending transaction node fields do not match schema v1")
    base_url = required_string(node, "base_url")
    try:
        normalized_base_url = normalize_base_url(base_url)
    except ValueError as error:
        raise CollectionError("pending transaction node origin is invalid") from error
    if normalized_base_url != base_url:
        raise CollectionError("pending transaction node origin is not normalized")
    role = required_string(node, "role")
    if role not in ROLES:
        raise CollectionError("pending transaction node role is unsupported")
    identity = NodeIdentity(
        base_url,
        required_identifier(node, "node_id"),
        role,
        required_identifier(node, "capture_profile"),
    )
    entry = require_object(root.get("artifact"), "pending transaction artifact")
    if (
        required_identifier(entry, "node_id") != identity.node_id
        or required_string(entry, "node_role") != identity.role
    ):
        raise CollectionError("pending transaction artifact belongs to another node")
    expected_name = hashlib.sha256(
        f"{identity.node_id}\0{required_identifier(entry, 'session_id')}".encode()
    ).hexdigest()
    if path.name != f"{expected_name}.json":
        raise CollectionError("pending transaction filename contradicts its identity")
    archive_path = _safe_index_archive_path(required_string(entry, "archive_path"))
    required_timestamp(entry, "collected_at_utc")
    required_timestamp(entry, "session_created_at_utc")
    required_nonnegative_integer(entry, "archive_bytes")
    required_sha256(entry, "archive_sha256")
    preview = entry.get("preview_bundle")
    if preview is not None:
        preview_object = require_object(preview, "pending transaction preview bundle")
        _safe_index_preview_path(required_string(preview_object, "directory"), archive_path)
        required_nonnegative_integer(preview_object, "frame_count")
        required_nonnegative_integer(preview_object, "mjpeg_bytes")
        required_sha256(preview_object, "mjpeg_sha256")
        required_sha256(preview_object, "pose_trace_sha256")
    return identity, entry


def _fsync_directory(directory: Path) -> None:
    descriptor = -1
    try:
        descriptor = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
        os.fsync(descriptor)
    except OSError as error:
        raise CollectionError(f"unable to sync directory {directory}: {error}") from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def _read_bounded_local_file(path: Path, maximum_bytes: int) -> bytes:
    try:
        with path.open("rb") as source:
            contents = source.read(maximum_bytes + 1)
    except OSError as error:
        raise CollectionError(f"unable to read local file {path}: {error}") from error
    if len(contents) > maximum_bytes:
        raise CollectionError(f"local file exceeds the configured bound: {path}")
    return contents


def _durable_mkdir(directory: Path) -> None:
    missing: list[Path] = []
    candidate = directory
    while not candidate.exists():
        missing.append(candidate)
        candidate = candidate.parent
    if candidate.is_symlink() or not candidate.is_dir():
        raise CollectionError(f"directory ancestor is not a real directory: {candidate}")
    for path in reversed(missing):
        try:
            path.mkdir()
        except OSError as error:
            raise CollectionError(f"unable to create directory {path}: {error}") from error
        _fsync_directory(path.parent)
    if directory.is_symlink() or not directory.is_dir():
        raise CollectionError(f"path is not a real directory: {directory}")


def _durable_unlink(path: Path) -> None:
    if path.is_symlink() or not path.is_file():
        raise CollectionError(f"durable unlink target is not a regular file: {path}")
    try:
        path.unlink()
    except OSError as error:
        raise CollectionError(f"unable to remove durable file {path}: {error}") from error
    _fsync_directory(path.parent)


def _cleanup_atomic_write_temps(destination: Path) -> None:
    if not destination.parent.exists():
        return
    pattern = re.compile(rf"\.{re.escape(destination.name)}\..+\.tmp")
    try:
        candidates = list(destination.parent.iterdir())
    except OSError as error:
        message = f"unable to inspect temporary files for {destination}: {error}"
        raise CollectionError(message) from error
    for candidate in candidates:
        if pattern.fullmatch(candidate.name) is not None:
            _durable_unlink(candidate)


def _atomic_write(destination: Path, contents: bytes) -> None:
    _durable_mkdir(destination.parent)
    _cleanup_atomic_write_temps(destination)
    descriptor = -1
    temporary_name = ""
    try:
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{destination.name}.",
            suffix=".tmp",
            dir=destination.parent,
        )
        with os.fdopen(descriptor, "wb") as output:
            descriptor = -1
            output.write(contents)
            output.flush()
            os.fsync(output.fileno())
        Path(temporary_name).replace(destination)
        _fsync_directory(destination.parent)
    except OSError as error:
        raise CollectionError(f"unable to atomically write {destination}: {error}") from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if temporary_name:
            try:  # noqa: SIM105
                Path(temporary_name).unlink(missing_ok=True)
            except OSError:
                pass


def _utc_timestamp(value: datetime.datetime) -> str:
    if value.tzinfo is None:
        raise CollectionError("collector clock must return a timezone-aware value")
    utc = value.astimezone(datetime.UTC)
    return utc.isoformat(timespec="microseconds").replace("+00:00", "Z")


def _compact_timestamp(value: str) -> str:
    parsed = datetime.datetime.fromisoformat(value.replace("Z", "+00:00"))
    return parsed.astimezone(datetime.UTC).strftime("%Y%m%dT%H%M%S.%fZ")
