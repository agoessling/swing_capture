"""Hermetic HTTP behavior tests for the local UI-lab server."""

from __future__ import annotations

import tempfile
import threading
import unittest
from functools import partial
from http.client import HTTPConnection, HTTPResponse
from http.server import ThreadingHTTPServer
from pathlib import Path
from typing import cast, final

from web.ui_lab_server import RangeRequestHandler


@final
class UiLabServerTest(unittest.TestCase):
    """Require browser-compatible full and partial static responses."""

    temporary: tempfile.TemporaryDirectory[str]  # pyright: ignore[reportUninitializedInstanceVariable]
    server: ThreadingHTTPServer  # pyright: ignore[reportUninitializedInstanceVariable]
    thread: threading.Thread  # pyright: ignore[reportUninitializedInstanceVariable]

    def setUp(self) -> None:  # pyright: ignore[reportImplicitOverride]
        """Start an isolated loopback server over one deterministic file."""
        self.temporary = tempfile.TemporaryDirectory()
        root = Path(self.temporary.name)
        (root / "fixture.mp4").write_bytes(b"0123456789")
        handler = partial(RangeRequestHandler, directory=str(root))
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self) -> None:  # pyright: ignore[reportImplicitOverride]
        """Stop the loopback server and remove its temporary document root."""
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temporary.cleanup()

    def _request(self, range_header: str | None = None) -> HTTPResponse:
        address = cast("tuple[str, int]", self.server.server_address)
        connection = HTTPConnection(address[0], address[1], timeout=2)
        headers = {} if range_header is None else {"Range": range_header}
        connection.request("GET", "/fixture.mp4", headers=headers)
        self.addCleanup(connection.close)
        return connection.getresponse()

    def test_full_file_advertises_byte_ranges(self) -> None:
        """Return an ordinary file while advertising seek support."""
        response = self._request()
        self.assertEqual(200, response.status)
        self.assertEqual("bytes", response.getheader("Accept-Ranges"))
        self.assertEqual(b"0123456789", response.read())

    def test_bounded_open_and_suffix_ranges(self) -> None:
        """Honor the three single-range forms used by browsers."""
        cases = [
            ("bytes=2-5", "bytes 2-5/10", b"2345"),
            ("bytes=7-", "bytes 7-9/10", b"789"),
            ("bytes=-3", "bytes 7-9/10", b"789"),
        ]
        for requested, expected_header, expected_body in cases:
            with self.subTest(requested=requested):
                response = self._request(requested)
                self.assertEqual(206, response.status)
                self.assertEqual(expected_header, response.getheader("Content-Range"))
                self.assertEqual(expected_body, response.read())

    def test_unsatisfiable_range_returns_file_size(self) -> None:
        """Reject an out-of-bounds range with a useful content-range."""
        response = self._request("bytes=20-")
        self.assertEqual(416, response.status)
        self.assertEqual("bytes */10", response.getheader("Content-Range"))
        self.assertEqual(b"", response.read())


if __name__ == "__main__":
    unittest.main()
