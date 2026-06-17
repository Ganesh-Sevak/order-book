#!/usr/bin/env python3
"""Serve the static dashboard and stream replay artifacts over SSE."""

from __future__ import annotations

import argparse
import functools
import http.server
import pathlib
import socketserver
import time
from collections.abc import Iterable


def rows(path: pathlib.Path) -> Iterable[str]:
    if not path.exists():
        return
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                yield line


class ReplayHandler(http.server.SimpleHTTPRequestHandler):
    snapshots: pathlib.Path
    trades: pathlib.Path
    delay: float

    def do_GET(self) -> None:  # noqa: N802 - http.server uses this name.
        if self.path != "/events":
            super().do_GET()
            return

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()

        for row in rows(self.snapshots):
            self.wfile.write(f"data: {row}\n\n".encode("utf-8"))
            self.wfile.flush()
            time.sleep(self.delay)
        for row in rows(self.trades):
            self.wfile.write(f"data: {row}\n\n".encode("utf-8"))
            self.wfile.flush()
            time.sleep(self.delay)


class LocalThreadingHTTPServer(http.server.ThreadingHTTPServer):
    def server_bind(self) -> None:
        socketserver.TCPServer.server_bind(self)
        self.server_name = self.server_address[0]
        self.server_port = self.server_address[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshots", default="/tmp/orderbook_snapshots.jsonl")
    parser.add_argument("--trades", default="/tmp/orderbook_trades.jsonl")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--delay-ms", type=float, default=10.0)
    args = parser.parse_args()

    web_root = pathlib.Path(__file__).resolve().parents[1] / "web"
    ReplayHandler.snapshots = pathlib.Path(args.snapshots)
    ReplayHandler.trades = pathlib.Path(args.trades)
    ReplayHandler.delay = args.delay_ms / 1000.0
    handler = functools.partial(ReplayHandler, directory=str(web_root))

    server = LocalThreadingHTTPServer(("127.0.0.1", args.port), handler)
    print(f"http://127.0.0.1:{args.port}", flush=True)
    print(f"SSE: http://127.0.0.1:{args.port}/events", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
