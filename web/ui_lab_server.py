"""Serve the built fixture demo for local UI iteration."""

from __future__ import annotations

import argparse
import re
import shutil
import socket as socket_api
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import TYPE_CHECKING, BinaryIO, cast, final

from python.runfiles import runfiles

if TYPE_CHECKING:
    from socket import socket
    from socketserver import BaseServer

DEFAULT_PORT = 4173
_LOOPBACK_ADDRESS = "127.0.0.1"
_ALL_INTERFACES_ADDRESS = "0.0.0.0"  # noqa: S104 - --lan explicitly requests LAN exposure.
_MAX_PORT = 65_535
_RANGE_PATTERN = re.compile(r"bytes=(\d*)-(\d*)")


def _byte_range(header: str, size: int) -> tuple[int, int] | None:
    """Parse one HTTP byte range and return its inclusive bounds."""
    match = _RANGE_PATTERN.fullmatch(header.strip())
    if match is None or size == 0:
        return None

    raw_start, raw_end = match.groups()
    if not raw_start:
        suffix_length = int(raw_end) if raw_end else 0
        if suffix_length == 0:
            return None
        return max(0, size - suffix_length), size - 1

    start = int(raw_start)
    if start >= size:
        return None
    end = size - 1 if not raw_end else min(int(raw_end), size - 1)
    return (start, end) if end >= start else None


@final
class RangeRequestHandler(SimpleHTTPRequestHandler):
    """Static-file handler with the single byte ranges used by media elements."""

    protocol_version = "HTTP/1.1"

    def __init__(
        self,
        request: socket,
        client_address: tuple[str, int],
        server: BaseServer,
        *,
        directory: str,
    ) -> None:
        """Initialize one request with no selected partial response."""
        self._response_range: tuple[int, int] | None = None
        super().__init__(request, client_address, server, directory=directory)

    def end_headers(self) -> None:  # pyright: ignore[reportImplicitOverride]
        """Keep repeated local UI loads from reusing stale built assets."""
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def send_head(self) -> BinaryIO | None:  # pyright: ignore[reportImplicitOverride]
        """Open the requested file and emit full or partial response headers."""
        self._response_range = None
        path = Path(self.translate_path(self.path))
        if path.is_dir():
            return super().send_head()

        try:
            source = path.open("rb")
        except OSError:
            self.send_error(404, "File not found")
            return None

        try:
            metadata = path.stat()
            size = metadata.st_size
            requested_range = self.headers.get("Range")
            if requested_range is None:
                self.send_response(200)
                content_length = size
            else:
                parsed_range = _byte_range(requested_range, size)
                if parsed_range is None:
                    source.close()
                    self.send_response(416)
                    self.send_header("Accept-Ranges", "bytes")
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return None
                self._response_range = parsed_range
                start, end = parsed_range
                self.send_response(206)
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
                content_length = end - start + 1

            self.send_header("Content-Type", self.guess_type(str(path)))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(content_length))
            self.send_header("Last-Modified", self.date_time_string(metadata.st_mtime))
            self.end_headers()
        except Exception:
            source.close()
            raise
        else:
            return source

    def copyfile(  # pyright: ignore[reportIncompatibleMethodOverride, reportImplicitOverride]
        self,
        source: BinaryIO,
        outputfile: BinaryIO,
    ) -> None:
        """Copy only the selected range for a partial response."""
        if self._response_range is None:
            shutil.copyfileobj(source, outputfile)
            return

        start, end = self._response_range
        source.seek(start)
        remaining = end - start + 1
        while remaining:
            chunk = source.read(min(64 * 1024, remaining))
            if not chunk:
                break
            outputfile.write(chunk)
            remaining -= len(chunk)


def _fixture_directory() -> Path:
    resolver = runfiles.Create()
    if resolver is None:
        message = "Bazel runfiles resolver is unavailable"
        raise RuntimeError(message)
    resolved = resolver.Rlocation("_main/web/fixture_demo")
    if resolved is None:
        message = "Bazel runfiles do not contain //web:fixture_demo"
        raise RuntimeError(message)
    directory = Path(resolved)
    if not directory.is_dir():
        message = f"fixture demo runfile is not a directory: {directory}"
        raise RuntimeError(message)
    return directory


def _port(value: str) -> int:
    port = int(value)
    if not 0 <= port <= _MAX_PORT:
        message = "port must be between 0 and 65535"
        raise argparse.ArgumentTypeError(message)
    return port


def _lan_address() -> str:
    """Return the IPv4 address selected by the host's default LAN route."""
    try:
        with socket_api.socket(socket_api.AF_INET, socket_api.SOCK_DGRAM) as probe:
            # UDP connect selects a route without sending traffic. TEST-NET-1 avoids depending on
            # a reachable external service while still exercising the host's ordinary route table.
            probe.connect(("192.0.2.1", 9))
            address = cast("tuple[str, int]", probe.getsockname())[0]
    except OSError as caught:
        message = "cannot determine a LAN IPv4 address; connect this host to the LAN and retry"
        raise RuntimeError(message) from caught
    if address == _ALL_INTERFACES_ADDRESS or address.startswith("127."):
        message = f"default route selected a non-LAN address: {address}"
        raise RuntimeError(message)
    return address


@final
class _Arguments(argparse.Namespace):
    """Typed command-line values populated by argparse."""

    def __init__(self) -> None:
        """Initialize the default port before argparse applies overrides."""
        super().__init__()
        self.lan = False
        self.port = DEFAULT_PORT


def main() -> int:
    """Run the UI-lab server until interrupted."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--lan",
        action="store_true",
        help="bind all IPv4 interfaces and print the default-route LAN URL",
    )
    parser.add_argument("--port", type=_port, default=DEFAULT_PORT)
    arguments = parser.parse_args(namespace=_Arguments())

    handler = partial(RangeRequestHandler, directory=str(_fixture_directory()))
    bind_address = _ALL_INTERFACES_ADDRESS if arguments.lan else _LOOPBACK_ADDRESS
    advertised_address = _lan_address() if arguments.lan else _LOOPBACK_ADDRESS
    server = ThreadingHTTPServer((bind_address, arguments.port), handler)
    port = server.server_address[1]
    print(
        f"http://{advertised_address}:{port}/demo.html?ui_lab=1#review",
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
