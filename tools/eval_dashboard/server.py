#!/usr/bin/env python3
from __future__ import annotations

import argparse
from datetime import datetime
import http.server
import json
import os
from pathlib import Path
import time
from urllib.parse import parse_qs, urlparse


HEALTH_FILES = ("control_health.json", "control_supervisor.json", "control_runtime.json")
DEFAULT_RUNTIME_HOME = Path("release") / "runtime"
RUNTIME_FILE_METADATA = (
    ("control_health", Path("control_health.json")),
    ("control_supervisor", Path("control_supervisor.json")),
    ("control_runtime", Path("control_runtime.json")),
    ("live_csv", Path("logs") / "svg_mb_control_output.csv"),
    ("events", Path("logs") / "svg_mb_control_events.jsonl"),
)


def resolve_runtime_home(repo_root: Path, runtime_home: Path | None = None) -> Path:
    if runtime_home is None:
        runtime_home = DEFAULT_RUNTIME_HOME
    if runtime_home.is_absolute():
        return runtime_home.resolve()
    return (repo_root / runtime_home).resolve()


def file_metadata(path: Path, now: float | None = None) -> dict:
    payload = {
        "path": str(path),
        "exists": False,
        "size_bytes": 0,
        "modified_time": None,
        "age_seconds": None,
    }
    try:
        stat = path.stat()
    except OSError:
        return payload
    if not path.is_file():
        return payload
    timestamp_now = time.time() if now is None else now
    payload["exists"] = True
    payload["size_bytes"] = stat.st_size
    payload["modified_time"] = datetime.fromtimestamp(stat.st_mtime).astimezone().isoformat(
        timespec="seconds"
    )
    payload["age_seconds"] = round(max(0.0, timestamp_now - stat.st_mtime), 3)
    return payload


def build_health_payload(repo_root: Path, runtime_home: Path | None = None) -> dict:
    """Aggregate the runtime health sidecars into one tolerant view.

    Missing or unreadable files become ``null`` sections; the health state
    itself is sourced from control_health.json (written by the C++ --health
    evaluator) so the dashboard never re-derives health logic.
    """
    base = resolve_runtime_home(repo_root, runtime_home)
    now = time.time()

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
        "runtime_home": str(base),
        "runtime_home_exists": base.is_dir(),
        "files": {
            key: file_metadata(base / relative_path, now)
            for key, relative_path in RUNTIME_FILE_METADATA
        },
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
    runtime_home: Path

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
            path = self.runtime_home / "logs" / "svg_mb_control_output.csv"
            if not path.is_file():
                self.send_not_found(f"live CSV not found: {path}")
                return
            self.send_bytes(read_csv_tail(path, tail_bytes), "text/csv; charset=utf-8")
            return

        if parsed.path == "/api/health.json":
            payload = json.dumps(
                build_health_payload(self.repo_root, self.runtime_home)
            ).encode("utf-8")
            self.send_bytes(payload, "application/json; charset=utf-8")
            return

        if parsed.path == "/api/events-tail.jsonl":
            path = self.runtime_home / "logs" / "svg_mb_control_events.jsonl"
            if not path.is_file():
                self.send_not_found(f"events JSONL not found: {path}")
                return
            self.send_bytes(read_file_tail(path, tail_bytes), "application/x-ndjson; charset=utf-8")
            return

        super().do_GET()


def main() -> int:
    parser = argparse.ArgumentParser(description="Serve SVG-MB-Control eval dashboard.")
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument(
        "--runtime-home",
        type=Path,
        default=None,
        help="Runtime home to read. Relative paths resolve under --repo-root.",
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    runtime_home = resolve_runtime_home(repo_root, args.runtime_home)

    class Handler(EvalDashboardHandler):
        def __init__(self, *handler_args, **handler_kwargs):
            super().__init__(*handler_args, directory=str(repo_root), **handler_kwargs)

    Handler.repo_root = repo_root
    Handler.runtime_home = runtime_home

    server = http.server.ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"SVG-MB-Control eval dashboard: http://{args.host}:{args.port}/tools/eval_dashboard/")
    print(f"Serving repo root: {repo_root}")
    print(f"Reading runtime home: {runtime_home}")
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
