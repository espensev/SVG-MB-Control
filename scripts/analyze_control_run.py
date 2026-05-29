#!/usr/bin/env python3
"""Thin wrapper around native `svg-mb-control analyze` for a raw CSV.

Native `analyze ingest --csv` + `analyze report` own all control-run analysis
now (percentiles, GPU-envelope peak, loop-timing/process-resource stats,
per-channel boost totals, decision records, manifests). This script remains a
convenience for summarizing a bare, not-yet-ingested control-loop CSV (and an
optional events JSONL) without a runtime home: it ingests the CSV into a
temporary database with the in-repo svg-mb-control.exe and forwards that exe's
native `analyze report` output. See docs/SCRIPT_STACK_REVIEW.md.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]


def resolve_exe() -> Path:
    """Locate the in-repo svg-mb-control.exe (env override, then build/release)."""
    override = os.environ.get("SVG_MB_CONTROL_TEST_EXE")
    if override:
        return Path(override)
    for candidate in (
        REPO_ROOT / "build" / "x64-release" / "svg-mb-control.exe",
        REPO_ROOT / "release" / "svg-mb-control.exe",
    ):
        if candidate.is_file():
            return candidate
    raise SystemExit(
        "svg-mb-control.exe not found under build/x64-release or release; "
        "build the release first (scripts\\Build-Release.ps1) or set "
        "SVG_MB_CONTROL_TEST_EXE."
    )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Summarize a raw, not-yet-ingested control-loop CSV by ingesting it "
            "into a temporary database and running native `svg-mb-control analyze "
            "report`. Output is the native report (text or JSON)."
        )
    )
    parser.add_argument("--csv", required=True, type=Path, help="Control-loop CSV path.")
    parser.add_argument("--events", type=Path, help="Optional events JSONL path.")
    parser.add_argument(
        "--gpu-load-threshold-c",
        type=float,
        default=None,
        help="Optional GPU-envelope threshold for the GPU-response block.",
    )
    parser.add_argument(
        "--format",
        choices=("markdown", "json"),
        default="markdown",
        help="markdown -> native text report; json -> native JSON report.",
    )
    parser.add_argument("--out", type=Path, help="Write the report to this path.")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if not args.csv.is_file():
        print(f"CSV not found: {args.csv}", file=sys.stderr)
        return 2
    exe = resolve_exe()

    with tempfile.TemporaryDirectory() as td:
        db = Path(td) / "raw_csv.db"

        ingest = [
            str(exe), "analyze", "ingest",
            "--csv", str(args.csv),
            "--db", str(db),
            "--quiet",
        ]
        if args.events:
            ingest += ["--events", str(args.events)]
        result = subprocess.run(ingest, capture_output=True, text=True)
        if result.returncode != 0:
            sys.stderr.write(result.stdout)
            sys.stderr.write(result.stderr)
            return result.returncode

        report = [
            str(exe), "analyze", "report",
            "--db", str(db),
            "--no-decision-record",
        ]
        if args.gpu_load_threshold_c is not None:
            report += ["--gpu-load-threshold-c", str(args.gpu_load_threshold_c)]
        if args.format == "json":
            report += ["--json"]
        if args.out:
            report += ["--out", str(args.out)]
        result = subprocess.run(report, capture_output=True, text=True)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        return result.returncode


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
