#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import math
import re
import statistics
import sys
from pathlib import Path
from typing import Any


CHANNEL_SETPOINT_RE = re.compile(r"^channel(\d+)_setpoint_pct$")


def _data_lines(path: Path):
    with path.open("r", encoding="utf-8", newline="") as handle:
        for line in handle:
            if not line.strip() or line.startswith("#"):
                continue
            yield line


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    return list(csv.DictReader(_data_lines(path)))


def maybe_float(value: str | None) -> float | None:
    if value is None or value == "":
        return None
    try:
        parsed = float(value)
    except ValueError:
        return None
    if math.isnan(parsed) or math.isinf(parsed):
        return None
    return parsed


def maybe_int(value: str | None) -> int | None:
    parsed = maybe_float(value)
    if parsed is None:
        return None
    return int(parsed)


def numeric_column(rows: list[dict[str, str]], name: str) -> list[float]:
    values: list[float] = []
    for row in rows:
        parsed = maybe_float(row.get(name))
        if parsed is not None:
            values.append(parsed)
    return values


def percentile(values: list[float], pct: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * (pct / 100.0)
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] + (ordered[upper] - ordered[lower]) * fraction


def stats(values: list[float]) -> dict[str, float | int | None]:
    if not values:
        return {
            "count": 0,
            "avg": None,
            "min": None,
            "p50": None,
            "p90": None,
            "p95": None,
            "p99": None,
            "max": None,
        }
    return {
        "count": len(values),
        "avg": statistics.fmean(values),
        "min": min(values),
        "p50": percentile(values, 50.0),
        "p90": percentile(values, 90.0),
        "p95": percentile(values, 95.0),
        "p99": percentile(values, 99.0),
        "max": max(values),
    }


def fmt(value: float | int | None, digits: int = 2) -> str:
    if value is None:
        return "n/a"
    if isinstance(value, int):
        return str(value)
    return f"{value:.{digits}f}"


def parse_bool(value: str | None) -> bool:
    return (value or "").strip().lower() == "true"


def read_events(path: Path | None) -> tuple[list[dict[str, Any]], dict[str, int]]:
    if path is None or not path.is_file():
        return [], {}
    events: list[dict[str, Any]] = []
    counts: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            counts["invalid_json"] = counts.get("invalid_json", 0) + 1
            continue
        events.append(event)
        event_type = str(event.get("event_type", "unknown"))
        counts[event_type] = counts.get(event_type, 0) + 1
    return events, dict(sorted(counts.items()))


def sha256_file(path: Path | None) -> str | None:
    if path is None or not path.is_file():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def detect_channels(fieldnames: list[str] | None) -> list[int]:
    if not fieldnames:
        return []
    channels: set[int] = set()
    for field in fieldnames:
        match = CHANNEL_SETPOINT_RE.match(field)
        if match:
            channels.add(int(match.group(1)))
    return sorted(channels)


def estimate_duration_seconds(rows: list[dict[str, str]]) -> float | None:
    intervals = numeric_column(rows, "loop_achieved_interval_ms")
    if intervals:
        return sum(intervals) / 1000.0

    wall_times: list[dt.datetime] = []
    for row in rows:
        value = row.get("loop_started_wall_clock") or row.get("wall_clock")
        if not value:
            continue
        try:
            wall_times.append(dt.datetime.fromisoformat(value))
        except ValueError:
            continue
    if len(wall_times) >= 2:
        return (wall_times[-1] - wall_times[0]).total_seconds()
    return None


def summarize_channels(
    rows: list[dict[str, str]],
    channels: list[int],
    duration_seconds: float | None,
    drop_threshold_pct: float,
) -> dict[str, dict[str, Any]]:
    summaries: dict[str, dict[str, Any]] = {}
    duration_minutes = (
        duration_seconds / 60.0
        if duration_seconds is not None and duration_seconds > 0.0
        else None
    )
    for channel in channels:
        prefix = f"channel{channel}_"
        setpoints = numeric_column(rows, prefix + "setpoint_pct")
        boosts = numeric_column(rows, prefix + "thermal_pressure_boost_pct")
        total_writes = [
            value
            for row in rows
            if (value := maybe_int(row.get(prefix + "total_writes"))) is not None
        ]
        if not setpoints and not boosts and not total_writes:
            continue
        write_count = max(total_writes) - min(total_writes) if total_writes else 0
        writes_per_min = (
            write_count / duration_minutes
            if duration_minutes is not None
            else None
        )
        drops = [
            previous - current
            for previous, current in zip(setpoints, setpoints[1:])
            if previous - current >= drop_threshold_pct
        ]
        boost_max = max(boosts) if boosts else None
        boost_cap_rows = 0
        if boost_max is not None:
            boost_cap_rows = sum(
                1 for value in boosts if math.isclose(value, boost_max, abs_tol=1e-6)
            )
        summaries[str(channel)] = {
            "setpoint": stats(setpoints),
            "final_total_writes": max(total_writes) if total_writes else 0,
            "write_count_delta": write_count,
            "writes_per_min": writes_per_min,
            "thermal_pressure_boost": stats(boosts),
            "boost_cap_rows": boost_cap_rows,
            "large_down_steps": len(drops),
            "max_down_step_pct": max(drops) if drops else None,
        }
    return summaries


def summarize(rows: list[dict[str, str]], events_path: Path | None, args) -> dict[str, Any]:
    events, event_counts = read_events(events_path)
    fieldnames = list(rows[0].keys()) if rows else []
    channels = detect_channels(fieldnames)
    duration_seconds = estimate_duration_seconds(rows)
    overrun_count = sum(1 for row in rows if parse_bool(row.get("loop_overrun")))
    return {
        "schema": "svg_mb_control.run_summary.v1",
        "csv_path": str(args.csv),
        "events_path": str(events_path) if events_path else None,
        "row_count": len(rows),
        "duration_seconds": duration_seconds,
        "profile": args.profile,
        "notes": args.notes,
        "temperatures": {
            "cpu_tctl_c": stats(numeric_column(rows, "cpu_tctl_c")),
            "cpu_max_c": stats(numeric_column(rows, "cpu_max_c")),
            "gpu_core_c": stats(numeric_column(rows, "gpu_core_c")),
            "gpu_memjn_c": stats(numeric_column(rows, "gpu_memjn_c")),
            "gpu_hotspot_c": stats(numeric_column(rows, "gpu_hotspot_c")),
        },
        "timing": {
            "loop_achieved_interval_ms": stats(
                numeric_column(rows, "loop_achieved_interval_ms")
            ),
            "loop_work_duration_ms": stats(
                numeric_column(rows, "loop_work_duration_ms")
            ),
            "loop_slip_ms": stats(numeric_column(rows, "loop_slip_ms")),
            "overrun_count": overrun_count,
        },
        "resources": {
            "process_cpu_pct": stats(numeric_column(rows, "process_cpu_pct")),
            "process_cpu_delta_ms": stats(
                numeric_column(rows, "process_cpu_delta_ms")
            ),
            "process_working_set_bytes": stats(
                numeric_column(rows, "process_working_set_bytes")
            ),
            "process_private_bytes": stats(
                numeric_column(rows, "process_private_bytes")
            ),
        },
        "channels": summarize_channels(
            rows, channels, duration_seconds, args.drop_threshold_pct
        ),
        "events": {
            "count": len(events),
            "by_type": event_counts,
        },
    }


def markdown_table(headers: list[str], rows: list[list[str]]) -> list[str]:
    output = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join("---" for _ in headers) + " |",
    ]
    for row in rows:
        output.append("| " + " | ".join(row) + " |")
    return output


def render_markdown(summary: dict[str, Any]) -> str:
    lines: list[str] = ["# SVG-MB-Control Run Summary", ""]
    lines.append(f"- CSV: `{summary['csv_path']}`")
    if summary.get("events_path"):
        lines.append(f"- Events: `{summary['events_path']}`")
    lines.append(f"- Rows: {summary['row_count']}")
    lines.append(f"- Duration: {fmt(summary['duration_seconds'])} s")
    if summary.get("profile"):
        lines.append(f"- Profile: {summary['profile']}")
    if summary.get("notes"):
        lines.append(f"- Notes: {summary['notes']}")
    lines.append("")

    temp_rows: list[list[str]] = []
    for name, stat in summary["temperatures"].items():
        temp_rows.append([
            name,
            fmt(stat["p50"]),
            fmt(stat["p90"]),
            fmt(stat["p99"]),
            fmt(stat["max"]),
        ])
    lines.extend(markdown_table(["Temperature", "p50", "p90", "p99", "max"], temp_rows))
    lines.append("")

    timing = summary["timing"]
    timing_rows = [
        [
            "achieved interval ms",
            fmt(timing["loop_achieved_interval_ms"]["p50"]),
            fmt(timing["loop_achieved_interval_ms"]["p95"]),
            fmt(timing["loop_achieved_interval_ms"]["max"]),
        ],
        [
            "loop work ms",
            fmt(timing["loop_work_duration_ms"]["p50"]),
            fmt(timing["loop_work_duration_ms"]["p95"]),
            fmt(timing["loop_work_duration_ms"]["max"]),
        ],
        [
            "loop slip ms",
            fmt(timing["loop_slip_ms"]["p50"]),
            fmt(timing["loop_slip_ms"]["p95"]),
            fmt(timing["loop_slip_ms"]["max"]),
        ],
    ]
    lines.extend(markdown_table(["Timing", "p50", "p95", "max"], timing_rows))
    lines.append(f"- Overrun rows: {timing['overrun_count']}")
    lines.append("")

    resources = summary["resources"]
    resource_rows = [
        [
            "process cpu pct",
            fmt(resources["process_cpu_pct"]["avg"], 3),
            fmt(resources["process_cpu_pct"]["max"], 3),
        ],
        [
            "working set bytes",
            fmt(resources["process_working_set_bytes"]["avg"], 0),
            fmt(resources["process_working_set_bytes"]["max"], 0),
        ],
        [
            "private bytes",
            fmt(resources["process_private_bytes"]["avg"], 0),
            fmt(resources["process_private_bytes"]["max"], 0),
        ],
    ]
    lines.extend(markdown_table(["Resource", "avg", "max"], resource_rows))
    lines.append("")

    channel_rows: list[list[str]] = []
    for channel, data in summary["channels"].items():
        channel_rows.append([
            channel,
            fmt(data["setpoint"]["p50"]),
            fmt(data["setpoint"]["p90"]),
            fmt(data["setpoint"]["max"]),
            str(data["final_total_writes"]),
            fmt(data["writes_per_min"], 2),
            fmt(data["thermal_pressure_boost"]["max"]),
            str(data["large_down_steps"]),
            fmt(data["max_down_step_pct"]),
        ])
    if channel_rows:
        lines.extend(markdown_table(
            [
                "Channel",
                "set p50",
                "set p90",
                "set max",
                "writes",
                "writes/min",
                "boost max",
                "drops",
                "max drop",
            ],
            channel_rows,
        ))
        lines.append("")

    event_rows = [
        [event_type, str(count)]
        for event_type, count in summary["events"]["by_type"].items()
    ]
    lines.append(f"- Event rows: {summary['events']['count']}")
    if event_rows:
        lines.extend(markdown_table(["Event type", "count"], event_rows))
    lines.append("")
    return "\n".join(lines)


def build_manifest(summary: dict[str, Any], args, summary_path: Path | None) -> dict[str, Any]:
    created_at = dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat()
    run_id = args.run_id or Path(args.csv).stem
    files = {
        "csv": args.csv,
        "events": args.events,
        "status": args.status,
        "current_state": args.current_state,
        "config": args.config,
        "build_info": args.build_info,
        "summary": summary_path,
    }
    return {
        "schema": "svg_mb_control.analysis_manifest.v1",
        "run_id": run_id,
        "created_at": created_at,
        "profile": args.profile,
        "notes": args.notes,
        "artifacts": {
            key: {
                "path": str(path) if path else None,
                "sha256": sha256_file(path) if path else None,
            }
            for key, path in files.items()
        },
        "row_count": summary["row_count"],
        "event_count": summary["events"]["count"],
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Summarize an SVG-MB-Control control-loop CSV and event log."
    )
    parser.add_argument("--csv", required=True, type=Path, help="Control-loop CSV path.")
    parser.add_argument("--events", type=Path, help="Runtime JSONL event path.")
    parser.add_argument("--status", type=Path, help="control_runtime.json captured with the run.")
    parser.add_argument("--current-state", type=Path, help="current_state.json captured with the run.")
    parser.add_argument("--config", type=Path, help="Control config used for the run.")
    parser.add_argument("--build-info", type=Path, help="Optional build info artifact.")
    parser.add_argument("--profile", default="", help="Run profile label.")
    parser.add_argument("--notes", default="", help="Operator notes.")
    parser.add_argument("--run-id", default="", help="Stable run id for the manifest.")
    parser.add_argument(
        "--drop-threshold-pct",
        type=float,
        default=0.35,
        help="Minimum downward setpoint step counted as a drop.",
    )
    parser.add_argument(
        "--format",
        choices=("markdown", "json"),
        default="markdown",
        help="Summary output format.",
    )
    parser.add_argument("--out", type=Path, help="Write summary to this path.")
    parser.add_argument("--manifest-out", type=Path, help="Write run manifest JSON here.")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if not args.csv.is_file():
        print(f"CSV not found: {args.csv}", file=sys.stderr)
        return 2

    rows = read_csv_rows(args.csv)
    summary = summarize(rows, args.events, args)
    if args.format == "json":
        output = json.dumps(summary, indent=2)
    else:
        output = render_markdown(summary)

    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(output, encoding="utf-8")
    else:
        print(output)

    if args.manifest_out:
        args.manifest_out.parent.mkdir(parents=True, exist_ok=True)
        manifest = build_manifest(summary, args, args.out)
        args.manifest_out.write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
