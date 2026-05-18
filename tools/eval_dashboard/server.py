#!/usr/bin/env python3
from __future__ import annotations

import argparse
import http.server
import json
import os
from pathlib import Path
from urllib.parse import parse_qs, urlparse


HEALTH_FILES = ("control_health.json", "control_supervisor.json", "control_runtime.json")


def build_health_payload(repo_root: Path) -> dict:
    """Aggregate the runtime health sidecars into one tolerant view.

    Missing or unreadable files become ``null`` sections; the health state
    itself is sourced from control_health.json (written by the C++ --health
    evaluator) so the dashboard never re-derives health logic.
    """
    base = repo_root / "release" / "runtime"

    def load(name: str) -> object:
        try:
            return json.loads((base / name).read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None

    health = load("control_health.json")
    supervisor = load("control_supervisor.json")
    runtime = load("control_runtime.json")
    runtime_view = None
    if isinstance(runtime, dict):
        runtime_view = {
            key: runtime.get(key)
            for key in (
                "mode",
                "status",
                "status_detail",
                "process_id",
                "loop_last_evaluation",
                "last_successful_restore_time",
            )
        }
    return {
        "available": health is not None or runtime is not None,
        "health": health,
        "supervisor": supervisor,
        "runtime": runtime_view,
    }


DEFAULT_TAIL_BYTES = 8 * 1024 * 1024
MAX_TAIL_BYTES = 64 * 1024 * 1024


def clamp_tail_bytes(raw: str | None) -> int:
    if not raw:
        return DEFAULT_TAIL_BYTES
    try:
        value = int(raw)
    except ValueError:
        return DEFAULT_TAIL_BYTES
    return max(64 * 1024, min(MAX_TAIL_BYTES, value))


def read_csv_tail(path: Path, tail_bytes: int) -> bytes:
    with path.open("rb") as handle:
        prologue: list[bytes] = []
        header = b""
        while True:
            line = handle.readline()
            if not line:
                break
            if line.startswith(b"#") or not line.strip():
                prologue.append(line)
                continue
            header = line
            break

        header_end = handle.tell()
        handle.seek(0, os.SEEK_END)
        file_size = handle.tell()
        start = max(header_end, file_size - tail_bytes)
        handle.seek(start)
        if start > header_end:
            handle.readline()
        body = handle.read()

    if not header:
        return b""
    return b"".join(prologue + [header, body])


def read_file_tail(path: Path, tail_bytes: int) -> bytes:
    with path.open("rb") as handle:
        handle.seek(0, os.SEEK_END)
        file_size = handle.tell()
        start = max(0, file_size - tail_bytes)
        handle.seek(start)
        if start > 0:
            handle.readline()
        return handle.read()


class EvalDashboardHandler(http.server.SimpleHTTPRequestHandler):
    repo_root: Path

    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def redirect_dashboard(self) -> None:
        self.send_response(302)
        self.send_header("Location", "/tools/eval_dashboard/")
        self.end_headers()

    def send_bytes(self, payload: bytes, content_type: str) -> None:
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def send_not_found(self, detail: str) -> None:
        payload = detail.encode("utf-8", errors="replace")
        self.send_response(404)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self) -> None:  # noqa: N802 - stdlib handler API
        parsed = urlparse(self.path)
        if parsed.path == "/":
            self.redirect_dashboard()
            return

        params = parse_qs(parsed.query)
        tail_bytes = clamp_tail_bytes(params.get("bytes", [None])[0])

        if parsed.path == "/api/live-tail.csv":
            path = self.repo_root / "release" / "runtime" / "logs" / "svg_mb_control_output.csv"
            if not path.is_file():
                self.send_not_found(f"live CSV not found: {path}")
                return
            self.send_bytes(read_csv_tail(path, tail_bytes), "text/csv; charset=utf-8")
            return

        if parsed.path == "/api/health.json":
            payload = json.dumps(build_health_payload(self.repo_root)).encode("utf-8")
            self.send_bytes(payload, "application/json; charset=utf-8")
            return

        if parsed.path == "/api/events-tail.jsonl":
            path = self.repo_root / "release" / "runtime" / "logs" / "svg_mb_control_events.jsonl"
            if not path.is_file():
                self.send_not_found(f"events JSONL not found: {path}")
                return
            self.send_bytes(read_file_tail(path, tail_bytes), "application/x-ndjson; charset=utf-8")
            return

        super().do_GET()


def main() -> int:
    parser = argparse.ArgumentParser(description="Serve SVG-MB-Control eval dashboard.")
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()

    class Handler(EvalDashboardHandler):
        def __init__(self, *handler_args, **handler_kwargs):
            super().__init__(*handler_args, directory=str(repo_root), **handler_kwargs)

    Handler.repo_root = repo_root

    server = http.server.ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"SVG-MB-Control eval dashboard: http://{args.host}:{args.port}/tools/eval_dashboard/")
    print(f"Serving repo root: {repo_root}")
    print("Press Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
