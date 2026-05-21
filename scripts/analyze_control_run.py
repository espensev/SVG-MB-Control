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


def read_csv_prologue(path: Path) -> dict[str, str]:
    fields: dict[str, str] = {}
    with path.open("r", encoding="utf-8", newline="") as handle:
        for line in handle:
            if not line.startswith("#"):
                break
            text = line[1:].strip()
            if not text:
                continue
            if "=" not in text:
                fields[text] = ""
                continue
            key, value = text.split("=", 1)
            fields[key.strip()] = value.strip()
    return fields


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


def text_counts(rows: list[dict[str, str]], name: str) -> dict[str, int]:
    counts: dict[str, int] = {}
    for row in rows:
        value = (row.get(name) or "").strip()
        if not value:
            continue
        counts[value] = counts.get(value, 0) + 1
    return dict(sorted(counts.items()))


RESPONSE_BOOST_SUFFIXES = (
    "thermal_pressure_boost_pct",
    "midband_pressure_boost_pct",
    "gpu_airflow_boost_pct",
    "cpu_low_soak_boost_pct",
)


def row_response_boost(row: dict[str, str], prefix: str) -> float | None:
    total = 0.0
    seen = False
    for suffix in RESPONSE_BOOST_SUFFIXES:
        parsed = maybe_float(row.get(prefix + suffix))
        if parsed is not None:
            total += parsed
            seen = True
    low_band = maybe_float(row.get(prefix + "low_band_effective_boost_pct"))
    if low_band is None:
        low_band = maybe_float(row.get(prefix + "low_band_stage_boost_pct"))
    if low_band is not None:
        total += low_band
        seen = True
    return total if seen else None


def response_boost_column(rows: list[dict[str, str]], prefix: str) -> list[float]:
    values: list[float] = []
    for row in rows:
        parsed = row_response_boost(row, prefix)
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


def maybe_bool(value: str | None) -> bool | None:
    if value is None or value == "":
        return None
    normalized = value.strip().lower()
    if normalized == "true":
        return True
    if normalized == "false":
        return False
    return None


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


def event_field_counts(events: list[dict[str, Any]], key: str) -> dict[str, int]:
    counts: dict[str, int] = {}
    for event in events:
        value = str(event.get(key, "")).strip()
        if not value:
            continue
        counts[value] = counts.get(value, 0) + 1
    return dict(sorted(counts.items()))


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


def gpu_envelope_c(core_c: float | None,
                   memjn_c: float | None,
                   hotspot_c: float | None) -> float | None:
    values = [value for value in (core_c, memjn_c) if value is not None]
    if hotspot_c is not None and hotspot_c > 0.0:
        values.append(hotspot_c)
    if not values:
        return None
    return max(values)


def gpu_envelope_for_row(row: dict[str, str]) -> float | None:
    explicit = maybe_float(row.get("gpu_envelope_c"))
    if explicit is not None:
        return explicit
    return gpu_envelope_c(
        maybe_float(row.get("gpu_core_c")),
        maybe_float(row.get("gpu_memjn_c")),
        maybe_float(row.get("gpu_hotspot_c")),
    )


def gpu_envelope_column(rows: list[dict[str, str]]) -> list[float]:
    values: list[float] = []
    for row in rows:
        envelope = gpu_envelope_for_row(row)
        if envelope is not None:
            values.append(envelope)
    return values


def row_interval_seconds(row: dict[str, str]) -> float | None:
    interval_ms = maybe_float(row.get("loop_achieved_interval_ms"))
    if interval_ms is None or interval_ms < 0.0:
        return None
    return interval_ms / 1000.0


def parse_row_wall_time(row: dict[str, str]) -> dt.datetime | None:
    value = row.get("loop_started_wall_clock") or row.get("wall_clock")
    if not value:
        return None
    try:
        return dt.datetime.fromisoformat(value)
    except ValueError:
        return None


def elapsed_seconds_by_row(rows: list[dict[str, str]]) -> list[float | None]:
    intervals = [row_interval_seconds(row) for row in rows]
    if any(interval is not None for interval in intervals):
        elapsed: list[float | None] = []
        current = 0.0
        for interval in intervals:
            elapsed.append(current)
            if interval is not None:
                current += interval
        return elapsed

    wall_times = [parse_row_wall_time(row) for row in rows]
    base = next((value for value in wall_times if value is not None), None)
    if base is None:
        return [None for _ in rows]
    return [
        (value - base).total_seconds() if value is not None else None
        for value in wall_times
    ]


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
        thermal_boosts = numeric_column(
            rows, prefix + "thermal_pressure_boost_pct"
        )
        boosts = response_boost_column(rows, prefix)
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
            "thermal_pressure_boost": stats(thermal_boosts),
            "response_boost": stats(boosts),
            "boost_cap_rows": boost_cap_rows,
            "large_down_steps": len(drops),
            "max_down_step_pct": max(drops) if drops else None,
            "response_sources": text_counts(rows, prefix + "response_source"),
            "write_reasons": text_counts(rows, prefix + "write_reason"),
        }
    return summaries


def summarize_gpu_response(
    rows: list[dict[str, str]],
    channels: list[int],
    threshold_c: float | None,
) -> dict[str, Any]:
    envelope_rows = [
        (index, envelope)
        for index, row in enumerate(rows)
        if (envelope := gpu_envelope_for_row(row)) is not None
    ]
    envelope_values = [envelope for _, envelope in envelope_rows]
    elapsed = elapsed_seconds_by_row(rows)

    peak: dict[str, Any] | None = None
    peak_index: int | None = None
    if envelope_rows:
        peak_index, peak_value = max(envelope_rows, key=lambda item: item[1])
        peak = {
            "row_index": peak_index,
            "row_number": peak_index + 1,
            "elapsed_seconds": elapsed[peak_index],
            "value_c": peak_value,
        }

    above_threshold_indices: list[int] = []
    above_threshold_seconds: float | None = None
    time_to_threshold_seconds: float | None = None
    if threshold_c is not None:
        above_threshold_indices = [
            index for index, envelope in envelope_rows if envelope >= threshold_c
        ]
        above_threshold_seconds = sum(
            row_interval_seconds(rows[index]) or 0.0
            for index in above_threshold_indices
        )
        if above_threshold_indices:
            time_to_threshold_seconds = elapsed[above_threshold_indices[0]]

    channel_response: dict[str, dict[str, Any]] = {}
    if peak_index is not None:
        peak_row = rows[peak_index]
        threshold_set = set(above_threshold_indices)
        for channel in channels:
            prefix = f"channel{channel}_"
            load_setpoints = [
                maybe_float(rows[index].get(prefix + "setpoint_pct"))
                for index in above_threshold_indices
                if maybe_float(rows[index].get(prefix + "setpoint_pct")) is not None
            ]
            channel_response[str(channel)] = {
                "setpoint_at_peak_pct": maybe_float(
                    peak_row.get(prefix + "setpoint_pct")
                ),
                "boost_at_peak_pct": row_response_boost(peak_row, prefix),
                "total_writes_at_peak": maybe_int(
                    peak_row.get(prefix + "total_writes")
                ),
                "write_active_at_peak": maybe_bool(
                    peak_row.get(prefix + "write_active")
                ),
                "load_row_count": sum(
                    1
                    for index in threshold_set
                    if maybe_float(rows[index].get(prefix + "setpoint_pct"))
                    is not None
                ),
                "setpoint_during_load": stats(load_setpoints),
            }

    return {
        "envelope_c": stats(envelope_values),
        "threshold_c": threshold_c,
        "above_threshold_rows": (
            len(above_threshold_indices) if threshold_c is not None else None
        ),
        "above_threshold_seconds": above_threshold_seconds,
        "time_to_threshold_seconds": time_to_threshold_seconds,
        "peak": peak,
        "channels": channel_response,
    }


def summarize(rows: list[dict[str, str]], events_path: Path | None, args) -> dict[str, Any]:
    events, event_counts = read_events(events_path)
    fieldnames = list(rows[0].keys()) if rows else []
    channels = detect_channels(fieldnames)
    duration_seconds = estimate_duration_seconds(rows)
    overrun_count = sum(1 for row in rows if parse_bool(row.get("loop_overrun")))
    return {
        "schema": "svg_mb_control.run_summary.v1",
        "csv_path": str(args.csv),
        "csv_prologue": read_csv_prologue(args.csv),
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
            "gpu_envelope_c": stats(gpu_envelope_column(rows)),
        },
        "gpu_response": summarize_gpu_response(
            rows, channels, args.gpu_load_threshold_c
        ),
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
            "by_severity": event_field_counts(events, "severity"),
            "by_error_code": event_field_counts(events, "error_code"),
        },
    }


def markdown_cell(value: Any) -> str:
    text = "n/a" if value is None else str(value)
    return text.replace("\n", " ").replace("|", "\\|")


def markdown_table(headers: list[str], rows: list[list[str]]) -> list[str]:
    output = [
        "| " + " | ".join(markdown_cell(header) for header in headers) + " |",
        "| " + " | ".join("---" for _ in headers) + " |",
    ]
    for row in rows:
        output.append("| " + " | ".join(markdown_cell(cell) for cell in row) + " |")
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

    gpu_response = summary["gpu_response"]
    peak = gpu_response.get("peak")
    if peak:
        lines.append(
            "- GPU envelope peak: "
            f"{fmt(peak['value_c'])} C at row {peak['row_number']} "
            f"({fmt(peak['elapsed_seconds'])} s)"
        )
        if gpu_response.get("threshold_c") is not None:
            lines.append(
                "- GPU load threshold: "
                f"{fmt(gpu_response['threshold_c'])} C, "
                f"rows above/equal: {gpu_response['above_threshold_rows']}, "
                f"time above/equal: "
                f"{fmt(gpu_response['above_threshold_seconds'])} s, "
                f"first crossing: "
                f"{fmt(gpu_response['time_to_threshold_seconds'])} s"
            )
        gpu_channel_rows: list[list[str]] = []
        for channel, data in gpu_response["channels"].items():
            gpu_channel_rows.append([
                channel,
                fmt(data["setpoint_at_peak_pct"]),
                fmt(data["boost_at_peak_pct"]),
                fmt(data["total_writes_at_peak"], 0),
                fmt(data["setpoint_during_load"]["p90"]),
                fmt(data["setpoint_during_load"]["max"]),
            ])
        if gpu_channel_rows:
            lines.extend(markdown_table(
                [
                    "Channel",
                    "set at GPU peak",
                    "boost at peak",
                    "writes at peak",
                    "load set p90",
                    "load set max",
                ],
                gpu_channel_rows,
            ))
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
            fmt(data["response_boost"]["max"]),
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
    if summary["events"].get("by_severity"):
        lines.append(
            "- Event severity: "
            f"{fmt_counts(summary['events']['by_severity'])}"
        )
    error_codes = {
        key: value
        for key, value in summary["events"].get("by_error_code", {}).items()
        if key != "none"
    }
    if error_codes:
        lines.append(f"- Event error codes: {fmt_counts(error_codes)}")
    if event_rows:
        lines.extend(markdown_table(["Event type", "count"], event_rows))
    lines.append("")
    return "\n".join(lines)


def fmt_counts(counts: dict[str, int]) -> str:
    if not counts:
        return "n/a"
    return ", ".join(f"{key}:{value}" for key, value in counts.items())


def build_diagnostic_flags(summary: dict[str, Any], args) -> list[str]:
    flags: list[str] = []
    if summary["row_count"] == 0:
        flags.append("No CSV data rows were parsed.")

    overrun_count = summary["timing"]["overrun_count"]
    if overrun_count:
        flags.append(f"Loop overrun rows were present: {overrun_count}.")

    severity_counts = summary["events"].get("by_severity", {})
    for severity in ("critical", "error"):
        count = severity_counts.get(severity, 0)
        if count:
            flags.append(f"{severity.capitalize()} event rows were present: {count}.")

    concern_terms = (
        "failed",
        "failure",
        "refused",
        "circuit_breaker",
        "sensor_failure",
        "timeout",
        "invalid_json",
    )
    for event_type, count in summary["events"]["by_type"].items():
        if any(term in event_type for term in concern_terms):
            flags.append(f"Concerning event count: {event_type}={count}.")

    cpu_tctl_max = summary["temperatures"]["cpu_tctl_c"]["max"]
    cpu_max_max = summary["temperatures"]["cpu_max_c"]["max"]
    cpu_peak = cpu_tctl_max if cpu_tctl_max is not None else cpu_max_max
    if (
        cpu_peak is not None
        and args.cpu_load_threshold_c is not None
        and cpu_peak >= args.cpu_load_threshold_c
    ):
        low_channels = []
        no_write_channels = []
        for channel, data in summary["channels"].items():
            setpoint_max = data["setpoint"]["max"]
            if (
                setpoint_max is not None
                and setpoint_max <= args.low_response_setpoint_pct
            ):
                low_channels.append(channel)
            if data["write_count_delta"] == 0:
                no_write_channels.append(channel)
        if low_channels:
            flags.append(
                "CPU peak "
                f"{fmt(cpu_peak)} C met/exceeded {fmt(args.cpu_load_threshold_c)} C, "
                "while setpoint max stayed at/below "
                f"{fmt(args.low_response_setpoint_pct)}% on channels "
                f"{', '.join(low_channels)}."
            )
        if no_write_channels:
            flags.append(
                "CPU peak "
                f"{fmt(cpu_peak)} C met/exceeded {fmt(args.cpu_load_threshold_c)} C, "
                "but write-count delta stayed at zero on channels "
                f"{', '.join(no_write_channels)}."
            )

    gpu_response = summary["gpu_response"]
    if (
        gpu_response.get("threshold_c") is not None
        and gpu_response.get("above_threshold_rows")
    ):
        low_gpu_channels = []
        for channel, data in gpu_response["channels"].items():
            load_max = data["setpoint_during_load"]["max"]
            if load_max is not None and load_max <= args.low_response_setpoint_pct:
                low_gpu_channels.append(channel)
        if low_gpu_channels:
            flags.append(
                "GPU envelope met/exceeded "
                f"{fmt(gpu_response['threshold_c'])} C, while load setpoint max "
                "stayed at/below "
                f"{fmt(args.low_response_setpoint_pct)}% on channels "
                f"{', '.join(low_gpu_channels)}."
            )

    if not flags:
        flags.append("No automatic risk flags triggered.")
    return flags


def build_decision_record(
    summary: dict[str, Any],
    manifest: dict[str, Any],
    args,
) -> str:
    lines: list[str] = ["# SVG-MB-Control Decision Record", ""]
    lines.append(f"- Run id: `{manifest['run_id']}`")
    lines.append(f"- Created: `{manifest['created_at']}`")
    lines.append(f"- Profile: {summary.get('profile') or 'n/a'}")
    lines.append(f"- Decision: {args.decision}")
    if args.hypothesis:
        lines.append(f"- Hypothesis: {args.hypothesis}")
    if summary.get("notes"):
        lines.append(f"- Notes: {summary['notes']}")
    if args.manifest_out:
        lines.append(f"- Analysis manifest: `{args.manifest_out}`")
    lines.append("")

    prologue = summary.get("csv_prologue") or {}
    identity_keys = [
        "schema",
        "mode",
        "session_start",
        "producer",
        "build_version",
        "git_hash",
        "config_path",
        "config_sha256",
        "runtime_policy_path",
        "runtime_policy_sha256",
        "control_poll_tick_ms",
        "control_write_cooldown_ms",
    ]
    identity_rows = [
        [key, prologue[key]]
        for key in identity_keys
        if prologue.get(key)
    ]
    if identity_rows:
        lines.append("## Source Identity")
        lines.extend(markdown_table(["Field", "Value"], identity_rows))
        lines.append("")

    artifact_rows: list[list[str]] = []
    for key in [
        "csv",
        "events",
        "status",
        "current_state",
        "config",
        "build_info",
        "summary",
    ]:
        artifact = manifest["artifacts"].get(key, {})
        artifact_rows.append([
            key,
            artifact.get("path") or "n/a",
            artifact.get("sha256") or "n/a",
        ])
    lines.append("## Artifacts")
    lines.extend(markdown_table(["Artifact", "Path", "SHA256"], artifact_rows))
    lines.append("")

    temperatures = summary["temperatures"]
    timing = summary["timing"]
    resources = summary["resources"]
    gpu_response = summary["gpu_response"]
    event_error_codes = {
        key: value
        for key, value in summary["events"].get("by_error_code", {}).items()
        if key != "none"
    }
    metric_rows = [
        ["Rows", str(summary["row_count"]), ""],
        ["Duration seconds", fmt(summary["duration_seconds"]), ""],
        [
            "CPU/Tctl p90/max C",
            f"{fmt(temperatures['cpu_tctl_c']['p90'])}/{fmt(temperatures['cpu_tctl_c']['max'])}",
            f"CPU threshold {fmt(args.cpu_load_threshold_c)} C",
        ],
        [
            "GPU envelope p90/max C",
            f"{fmt(temperatures['gpu_envelope_c']['p90'])}/{fmt(temperatures['gpu_envelope_c']['max'])}",
            f"GPU threshold {fmt(gpu_response.get('threshold_c'))} C",
        ],
        [
            "Achieved interval p95/max ms",
            f"{fmt(timing['loop_achieved_interval_ms']['p95'])}/{fmt(timing['loop_achieved_interval_ms']['max'])}",
            f"overruns {timing['overrun_count']}",
        ],
        [
            "Loop work p95/max ms",
            f"{fmt(timing['loop_work_duration_ms']['p95'])}/{fmt(timing['loop_work_duration_ms']['max'])}",
            "",
        ],
        [
            "Process CPU avg/max pct",
            f"{fmt(resources['process_cpu_pct']['avg'], 3)}/{fmt(resources['process_cpu_pct']['max'], 3)}",
            "",
        ],
        ["Event rows", str(summary["events"]["count"]), ""],
        [
            "Event severity",
            fmt_counts(summary["events"].get("by_severity", {})),
            "",
        ],
        ["Event error codes", fmt_counts(event_error_codes), ""],
    ]
    lines.append("## Metrics")
    lines.extend(markdown_table(["Metric", "Value", "Context"], metric_rows))
    lines.append("")

    channel_rows: list[list[str]] = []
    for channel, data in summary["channels"].items():
        channel_rows.append([
            channel,
            fmt(data["setpoint"]["p90"]),
            fmt(data["setpoint"]["max"]),
            str(data["write_count_delta"]),
            fmt(data["writes_per_min"], 2),
            fmt(data["response_boost"]["max"]),
            fmt_counts(data.get("response_sources", {})),
            fmt_counts(data.get("write_reasons", {})),
        ])
    if channel_rows:
        lines.append("## Channel Response")
        lines.extend(markdown_table(
            [
                "Channel",
                "set p90",
                "set max",
                "writes delta",
                "writes/min",
                "boost max",
                "response sources",
                "write reasons",
            ],
            channel_rows,
        ))
        lines.append("")

    flags = build_diagnostic_flags(summary, args)
    lines.append("## Automatic Flags")
    for flag in flags:
        lines.append(f"- {flag}")
    lines.append("")

    lines.append("## Operator Outcome")
    lines.append("- Result: pending")
    lines.append("- Follow-up: pending")
    lines.append("")
    return "\n".join(lines)


def build_manifest(
    summary: dict[str, Any],
    args,
    summary_path: Path | None,
    decision_record_path: Path | None,
) -> dict[str, Any]:
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
        "decision_record": decision_record_path,
    }
    return {
        "schema": "svg_mb_control.analysis_manifest.v1",
        "run_id": run_id,
        "created_at": created_at,
        "profile": args.profile,
        "notes": args.notes,
        "decision": args.decision,
        "hypothesis": args.hypothesis,
        "csv_prologue": summary.get("csv_prologue", {}),
        "analysis_thresholds": {
            "drop_threshold_pct": args.drop_threshold_pct,
            "gpu_load_threshold_c": args.gpu_load_threshold_c,
            "cpu_load_threshold_c": args.cpu_load_threshold_c,
            "low_response_setpoint_pct": args.low_response_setpoint_pct,
        },
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


def default_decision_record_path(args) -> Path | None:
    if args.out and args.format == "markdown":
        return args.out.with_name(f"{args.out.stem}.decision.md")
    return None


def resolve_decision_record_path(args) -> Path | None:
    if args.no_decision_record:
        return None
    if args.decision_record_out:
        value = args.decision_record_out.strip()
        if value.lower() == "auto":
            return default_decision_record_path(args) or args.csv.with_name(
                f"{args.csv.stem}.decision.md"
            )
        return Path(value)
    return default_decision_record_path(args)


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
        "--hypothesis",
        default="",
        help="Tuning hypothesis copied into the generated decision record.",
    )
    parser.add_argument(
        "--decision",
        default="pending operator review",
        help="Decision text copied into the generated decision record.",
    )
    parser.add_argument(
        "--drop-threshold-pct",
        type=float,
        default=0.35,
        help="Minimum downward setpoint step counted as a drop.",
    )
    parser.add_argument(
        "--gpu-load-threshold-c",
        type=float,
        default=None,
        help=(
            "Optional GPU envelope threshold used to mark load rows in the "
            "GPU response section."
        ),
    )
    parser.add_argument(
        "--cpu-load-threshold-c",
        type=float,
        default=75.0,
        help=(
            "CPU/Tctl threshold used by the generated decision record to flag "
            "high-temperature low-response runs."
        ),
    )
    parser.add_argument(
        "--low-response-setpoint-pct",
        type=float,
        default=35.0,
        help=(
            "Setpoint ceiling used by the generated decision record to flag "
            "low/no fan response during load."
        ),
    )
    parser.add_argument(
        "--format",
        choices=("markdown", "json"),
        default="markdown",
        help="Summary output format.",
    )
    parser.add_argument("--out", type=Path, help="Write summary to this path.")
    parser.add_argument("--manifest-out", type=Path, help="Write run manifest JSON here.")
    parser.add_argument(
        "--decision-record-out",
        help=(
            "Write a compact Markdown tuning decision record to this path. "
            "Use 'auto' to derive the path from --out or --csv."
        ),
    )
    parser.add_argument(
        "--no-decision-record",
        action="store_true",
        help="Suppress the automatic decision record normally written beside a Markdown --out file.",
    )
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

    decision_record_path = resolve_decision_record_path(args)
    manifest = (
        build_manifest(summary, args, args.out, decision_record_path)
        if decision_record_path or args.manifest_out
        else None
    )
    if decision_record_path:
        decision_record_path.parent.mkdir(parents=True, exist_ok=True)
        decision_record_path.write_text(
            build_decision_record(summary, manifest, args) + "\n",
            encoding="utf-8",
        )
        manifest["artifacts"]["decision_record"]["sha256"] = sha256_file(
            decision_record_path
        )

    if args.manifest_out:
        args.manifest_out.parent.mkdir(parents=True, exist_ok=True)
        args.manifest_out.write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
