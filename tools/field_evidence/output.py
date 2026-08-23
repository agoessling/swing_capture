"""Crash-safe, non-overwriting publication for immutable field evidence."""

from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path


def require_absent(path: Path) -> None:
    """Fail before expensive replay when an output path is already occupied."""
    if path.exists() or path.is_symlink():
        message = f"refusing to overwrite existing evidence {path}"
        raise FileExistsError(message)


def _synchronize_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def publish_json_exclusive(path: Path, value: object) -> None:
    """Atomically publish complete JSON while never replacing existing evidence."""
    rendered = json.dumps(value, indent=2, sort_keys=True) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="x",
            encoding="utf-8",
            dir=path.parent,
            prefix=f".{path.name}.partial.",
            delete=False,
        ) as temporary:
            temporary.write(rendered)
            temporary.flush()
            os.fsync(temporary.fileno())
            temporary_path = Path(temporary.name)
        try:
            os.link(temporary_path, path)
        except FileExistsError as error:
            message = f"refusing to overwrite existing evidence {path}"
            raise FileExistsError(message) from error
        _synchronize_directory(path.parent)
        temporary_path.unlink()
        temporary_path = None
        _synchronize_directory(path.parent)
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
