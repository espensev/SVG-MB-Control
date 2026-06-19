#!/usr/bin/env python3
"""Extract CPU/AIO response segments from control-loop CSV logs.

This is an offline/read-only helper for radiator-response evaluation. It does
not ingest into the native analysis DB and it does not change runtime state.
The output is intentionally compact: one JSON summary, one segment-index CSV,
and optional per-segment row extracts containing the columns needed to judge
whether AIO-radiator fan movement correlates with lower CPU temperature.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import sys
from collections import Counter
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_power_lead import derive_watts, tick_seconds  # noqa: E402
from control_csv import column, parse_control_csv, to_float  # noqa: E402

RADIATOR_CHANNELS = (1, 4, 5)
CONTEXT_CHANNELS = (0, 2, 3)
DELTA_THRESHOLDS_PCT = (0.25, 0.5, 1.0, 2.0, 3.0, 5.0)
FOLLOWUP_SECONDS = (30.0, 60.0, 120.0)


def _float_or_none(value: str | None) -> float | None:
    return to_float(value or "")


def _parse_thresholds(raw: str) -> list[float]:
    out: list[float] = []
    for part in raw.split(","):
        part = part.strip()
        if not part:
            continue
        out.append(float(part))
    return out


def _pct(values: list[float], q: float) -> float | None:
    xs = sorted(v for v in values if math.isfinite(v))
    if not xs:
        return None
    index = round(q * (len(xs) - 1))
    return xs[max(0, min(len(xs) - 1, index))]


def _stats(values: list[float | None]) -> dict[str, float | int | None]:
    xs = [v for v in values if v is not None and math.isfinite(v)]
    if not xs:
        return {"count": 0, "min": None, "p50": None, "p90": None,
                "max": None, "avg": None}
    return {
        "count": len(xs),
        "min": round(min(xs), 3),
        "p50": round(_pct(xs, 0.50), 3),
        "p90": round(_pct(xs, 0.90), 3),
        "max": round(max(xs), 3),
        "avg": round(statistics.fmean(xs), 3),
    }


def _round(value: float | None, digits: int = 3) -> float | None:
    return None if value is None or not math.isfinite(value) else round(value, digits)


def _first_existing(paths: list[Path]) -> Path | None:
    found = [p for p in paths if p.is_file()]
    if not found:
        return None
    return max(found, key=lambda p: p.stat().st_mtime)


def latest_control_csv(runtime_home: Path) -> Path:
    archive = runtime_home / "logs" / "archive"
    candidates = list(archive.glob("*control-loop*.csv"))
    live = runtime_home / "logs" / "svg_mb_control_output.csv"
    if live.is_file():
        candidates.append(live)
    latest = _first_existing(candidates)
    if latest is None:
        raise SystemExit(f"No control-loop CSV found under {runtime_home}")
    return latest


def _channel_value(records: list[dict[str, str]], channel: int,
                   suffix: str) -> list[float | None]:
    key = f"channel{channel}_{suffix}"
    return [_float_or_none(r.get(key)) for r in records]


def _avg_channels(records: list[dict[str, str]], channels: tuple[int, ...],
                  suffix: str) -> list[float | None]:
    per_channel = [_channel_value(records, ch, suffix) for ch in channels]
    out: list[float | None] = []
    for values in zip(*per_channel):
        xs = [v for v in values if v is not None]
        out.append(statistics.fmean(xs) if xs else None)
    return out


def _source_counts(records: list[dict[str, str]], indices: range,
                   channel: int) -> dict[str, int]:
    key = f"channel{channel}_response_source"
    counts = Counter(records[i].get(key) or "(blank)" for i in indices)
    return dict(sorted(counts.items()))


def load_csv(path: Path) -> dict:
    meta, header, rows = parse_control_csv(path)
    if "wall_clock" not in header or "cpu_tctl_c" not in header:
        raise SystemExit("CSV must contain wall_clock and cpu_tctl_c columns")
    records = [dict(zip(header, row)) for row in rows]
    wall = column(header, rows, "wall_clock")
    dt_s = tick_seconds(wall)
    cpu_w = derive_watts(
        column(header, rows, "cpu_power_sample_id"),
        column(header, rows, "cpu_pkg_energy_delta_uj"),
        column(header, rows, "cpu_power_window_ms"),
    )
    return {
        "meta": meta,
        "header": header,
        "records": records,
        "wall": wall,
        "tick_seconds": dt_s,
        "cpu_tctl_c": [_float_or_none(v) for v in column(header, rows, "cpu_tctl_c")],
        "cpu_max_c": [_float_or_none(v) for v in column(header, rows, "cpu_max_c")],
        "cpu_package_w": cpu_w,
        "system_cpu_busy_pct": [
            _float_or_none(v) for v in column(header, rows, "system_cpu_busy_pct")
        ],
        "gpu_memjn_c": [_float_or_none(v) for v in column(header, rows, "gpu_memjn_c")],
        "gpu_power_w": [
            (v / 1000.0 if v is not None else None)
            for v in [_float_or_none(x) for x in column(header, rows, "gpu_power_mw")]
        ],
    }


def _crossings(values: list[float | None], thresholds: list[float],
               kind: str) -> list[dict]:
    events: list[dict] = []
    for threshold in thresholds:
        prev: float | None = None
        for index, value in enumerate(values):
            if value is None:
                continue
            if prev is not None and prev < threshold <= value:
                events.append({
                    "index": index,
                    "kind": kind,
                    "threshold": threshold,
                    "value": value,
                })
            prev = value
    return events


def _prefer_event(events: list[dict]) -> dict:
    tctl = [e for e in events if e["kind"] == "cpu_tctl_c"]
    if tctl:
        return sorted(tctl, key=lambda e: (e["index"], -e["threshold"]))[0]
    return sorted(events, key=lambda e: (e["index"], -e["threshold"]))[0]


def find_event_clusters(data: dict, cpu_tctl_thresholds_c: list[float],
                        cpu_power_thresholds_w: list[float],
                        merge_within_s: float) -> list[dict]:
    dt_s = data["tick_seconds"]
    raw = _crossings(data["cpu_tctl_c"], cpu_tctl_thresholds_c, "cpu_tctl_c")
    raw.extend(_crossings(data["cpu_package_w"], cpu_power_thresholds_w,
                          "cpu_package_w"))
    raw.sort(key=lambda e: (e["index"], e["kind"], e["threshold"]))
    clusters: list[list[dict]] = []
    for event in raw:
        if not clusters:
            clusters.append([event])
            continue
        if (event["index"] - clusters[-1][0]["index"]) * dt_s <= merge_within_s:
            clusters[-1].append(event)
        else:
            clusters.append([event])
    return [{
        "events": cluster,
        "primary": _prefer_event(cluster),
        "first_index": min(e["index"] for e in cluster),
        "last_index": max(e["index"] for e in cluster),
    } for cluster in clusters]


def _indices_around(center: int, count: int, pre_s: float,
                    post_s: float, dt_s: float) -> range:
    pre = max(1, round(pre_s / dt_s))
    post = max(1, round(post_s / dt_s))
    start = max(0, center - pre)
    end = min(count, center + post + 1)
    return range(start, end)


def _slice_stats(series: list[float | None], indices: range) -> dict:
    return _stats([series[i] for i in indices])


def _time_to_delta(series: list[float | None], baseline: float | None,
                   event_index: int, indices: range, dt_s: float) -> dict[str, float | None]:
    out: dict[str, float | None] = {}
    for delta in DELTA_THRESHOLDS_PCT:
        hit = None
        if baseline is not None:
            for i in indices:
                value = series[i]
                if i >= event_index and value is not None and value >= baseline + delta:
                    hit = round((i - event_index) * dt_s, 3)
                    break
        out[str(delta)] = hit
    return out


def _followup(data: dict, series: list[float | None], baseline: float | None,
              event_index: int, indices: range, dt_s: float) -> dict:
    if baseline is None:
        return {}
    out: dict[str, dict] = {}
    valid = set(indices)
    for delta in DELTA_THRESHOLDS_PCT:
        hit_index = None
        for i in indices:
            value = series[i]
            if i >= event_index and value is not None and value >= baseline + delta:
                hit_index = i
                break
        if hit_index is None:
            continue
        hit_tctl = data["cpu_tctl_c"][hit_index]
        hit_w = data["cpu_package_w"][hit_index]
        detail = {
            "hit_s": round((hit_index - event_index) * dt_s, 3),
            "hit_cpu_tctl_c": _round(hit_tctl),
            "hit_cpu_package_w": _round(hit_w),
        }
        for seconds in FOLLOWUP_SECONDS:
            idx = hit_index + round(seconds / dt_s)
            key = f"after_{int(seconds)}s"
            if idx not in valid:
                detail[key] = None
                continue
            tctl = data["cpu_tctl_c"][idx]
            watts = data["cpu_package_w"][idx]
            detail[key] = {
                "cpu_tctl_c": _round(tctl),
                "cpu_tctl_delta_c": _round(
                    None if tctl is None or hit_tctl is None else tctl - hit_tctl),
                "cpu_package_w": _round(watts),
                "cpu_package_w_delta": _round(
                    None if watts is None or hit_w is None else watts - hit_w),
            }
        out[str(delta)] = detail
    return out


def summarize_segment(data: dict, cluster: dict, segment_id: int,
                      indices: range, gpu_power_confound_w: float,
                      gpu_mem_confound_c: float,
                      stop_at_gpu_confound: bool) -> dict:
    records = data["records"]
    dt_s = data["tick_seconds"]
    event = cluster["primary"]
    event_index = event["index"]
    pre = range(indices.start, event_index)
    post = range(event_index, indices.stop)
    rad_sp = _avg_channels(records, RADIATOR_CHANNELS, "setpoint_pct")
    rad_duty = _avg_channels(records, RADIATOR_CHANNELS, "duty_pct")
    rad_rpm = _avg_channels(records, RADIATOR_CHANNELS, "rpm")
    case_sp = _avg_channels(records, CONTEXT_CHANNELS, "setpoint_pct")

    first_gpu_confound = None
    first_gpu_confound_index = None
    for i in post:
        gpu_w = data["gpu_power_w"][i]
        gpu_mem = data["gpu_memjn_c"][i]
        if ((gpu_w is not None and gpu_w >= gpu_power_confound_w)
                or (gpu_mem is not None and gpu_mem >= gpu_mem_confound_c)):
            first_gpu_confound = round((i - event_index) * dt_s, 3)
            first_gpu_confound_index = i
            break
    metric_stop = indices.stop
    ended_by = "configured_post_window"
    if (stop_at_gpu_confound and first_gpu_confound_index is not None
            and first_gpu_confound_index > event_index):
        metric_stop = first_gpu_confound_index
        ended_by = "gpu_confound"
    elif stop_at_gpu_confound and first_gpu_confound_index == event_index:
        metric_stop = event_index
        ended_by = "gpu_confound_at_event"
    metric_post = range(event_index, metric_stop)
    metric_indices = range(indices.start, metric_stop)

    def event_value(name: str) -> float | None:
        return data[name][event_index]

    rad_base = rad_sp[event_index]
    case_base = case_sp[event_index]
    segment = {
        "segment_id": segment_id,
        "event": {
            "kind": event["kind"],
            "threshold": event["threshold"],
            "index": event_index,
            "wall_clock": records[event_index]["wall_clock"],
            "value": _round(event["value"]),
            "cluster_reasons": [
                {"kind": e["kind"], "threshold": e["threshold"],
                 "index": e["index"], "value": _round(e["value"])}
                for e in cluster["events"]
            ],
        },
        "window": {
            "start_index": indices.start,
            "end_index": indices.stop - 1,
            "start_wall_clock": records[indices.start]["wall_clock"],
            "end_wall_clock": records[indices.stop - 1]["wall_clock"],
            "pre_s": round((event_index - indices.start) * dt_s, 3),
            "post_s": round((indices.stop - 1 - event_index) * dt_s, 3),
            "tick_seconds": dt_s,
        },
        "analysis_window": {
            "post_start_index": event_index,
            "post_end_index": metric_stop - 1 if metric_stop > event_index else None,
            "post_s": round((metric_stop - event_index) * dt_s, 3),
            "ended_by": ended_by,
            "stop_at_gpu_confound": stop_at_gpu_confound,
        },
        "context": {
            "gpu_confounded": first_gpu_confound is not None,
            "first_gpu_confound_s": first_gpu_confound,
            "event_cpu_tctl_c": _round(event_value("cpu_tctl_c")),
            "event_cpu_package_w": _round(event_value("cpu_package_w")),
            "event_gpu_memjn_c": _round(event_value("gpu_memjn_c")),
            "event_gpu_board_w": _round(event_value("gpu_power_w")),
        },
        "signals": {
            "pre": {
                "cpu_tctl_c": _slice_stats(data["cpu_tctl_c"], pre),
                "cpu_package_w": _slice_stats(data["cpu_package_w"], pre),
                "gpu_memjn_c": _slice_stats(data["gpu_memjn_c"], pre),
                "gpu_power_w": _slice_stats(data["gpu_power_w"], pre),
            },
            "post": {
                "cpu_tctl_c": _slice_stats(data["cpu_tctl_c"], metric_post),
                "cpu_package_w": _slice_stats(data["cpu_package_w"], metric_post),
                "gpu_memjn_c": _slice_stats(data["gpu_memjn_c"], metric_post),
                "gpu_power_w": _slice_stats(data["gpu_power_w"], metric_post),
            },
        },
        "radiator_group": {
            "channels": list(RADIATOR_CHANNELS),
            "event_setpoint_avg_pct": _round(rad_base),
            "event_duty_avg_pct": _round(rad_duty[event_index]),
            "event_rpm_avg": _round(rad_rpm[event_index]),
            "setpoint_delta_max_pct": _round(
                None if rad_base is None else max(
                    (v - rad_base for v in (rad_sp[i] for i in metric_post)
                     if v is not None),
                    default=0.0)),
            "setpoint_delta_p90_pct": _round(
                None if rad_base is None else _pct(
                    [v - rad_base for v in (rad_sp[i] for i in metric_post)
                     if v is not None], 0.90)),
            "times_to_setpoint_delta_s": _time_to_delta(
                rad_sp, rad_base, event_index, metric_post, dt_s),
            "temperature_after_response": _followup(
                data, rad_sp, rad_base, event_index, metric_indices, dt_s),
        },
        "context_fans": {
            "channels": list(CONTEXT_CHANNELS),
            "event_setpoint_avg_pct": _round(case_base),
            "setpoint_delta_max_pct": _round(
                None if case_base is None else max(
                    (v - case_base for v in (case_sp[i] for i in metric_post)
                     if v is not None),
                    default=0.0)),
        },
        "channels": {},
    }

    for channel in (*RADIATOR_CHANNELS, *CONTEXT_CHANNELS):
        setpoint = _channel_value(records, channel, "setpoint_pct")
        duty = _channel_value(records, channel, "duty_pct")
        rpm = _channel_value(records, channel, "rpm")
        base = setpoint[event_index]
        segment["channels"][str(channel)] = {
            "event_setpoint_pct": _round(base),
            "event_duty_pct": _round(duty[event_index]),
            "event_rpm": _round(rpm[event_index]),
            "setpoint_delta_max_pct": _round(
                None if base is None else max(
                    (v - base for v in (setpoint[i] for i in metric_post)
                     if v is not None),
                    default=0.0)),
            "duty_delta_max_pct": _round(
                None if duty[event_index] is None else max(
                    (v - duty[event_index] for v in (duty[i] for i in metric_post)
                     if v is not None),
                    default=0.0)),
            "rpm_delta_max": _round(
                None if rpm[event_index] is None else max(
                    (v - rpm[event_index] for v in (rpm[i] for i in metric_post)
                     if v is not None),
                    default=0.0)),
            "times_to_setpoint_delta_s": _time_to_delta(
                setpoint, base, event_index, metric_post, dt_s),
            "response_source_counts": _source_counts(records, metric_post, channel),
        }
    return segment


SEGMENT_INDEX_FIELDS = [
    "segment_id", "event_wall_clock", "event_kind", "event_threshold",
    "event_cpu_tctl_c", "event_cpu_package_w", "event_gpu_memjn_c",
    "event_gpu_board_w", "gpu_confounded", "first_gpu_confound_s",
    "rad_event_setpoint_avg_pct", "rad_setpoint_delta_max_pct",
    "rad_time_to_plus_1pct_s", "rad_time_to_plus_2pct_s",
    "ch1_delta_max_pct", "ch4_delta_max_pct", "ch5_delta_max_pct",
    "ctx_delta_max_pct", "post_cpu_tctl_p90_c", "post_cpu_tctl_max_c",
    "post_cpu_package_w_p90",
]


def segment_index_row(segment: dict) -> dict[str, object]:
    rad = segment["radiator_group"]
    channels = segment["channels"]
    return {
        "segment_id": segment["segment_id"],
        "event_wall_clock": segment["event"]["wall_clock"],
        "event_kind": segment["event"]["kind"],
        "event_threshold": segment["event"]["threshold"],
        "event_cpu_tctl_c": segment["context"]["event_cpu_tctl_c"],
        "event_cpu_package_w": segment["context"]["event_cpu_package_w"],
        "event_gpu_memjn_c": segment["context"]["event_gpu_memjn_c"],
        "event_gpu_board_w": segment["context"]["event_gpu_board_w"],
        "gpu_confounded": segment["context"]["gpu_confounded"],
        "first_gpu_confound_s": segment["context"]["first_gpu_confound_s"],
        "rad_event_setpoint_avg_pct": rad["event_setpoint_avg_pct"],
        "rad_setpoint_delta_max_pct": rad["setpoint_delta_max_pct"],
        "rad_time_to_plus_1pct_s": rad["times_to_setpoint_delta_s"]["1.0"],
        "rad_time_to_plus_2pct_s": rad["times_to_setpoint_delta_s"]["2.0"],
        "ch1_delta_max_pct": channels["1"]["setpoint_delta_max_pct"],
        "ch4_delta_max_pct": channels["4"]["setpoint_delta_max_pct"],
        "ch5_delta_max_pct": channels["5"]["setpoint_delta_max_pct"],
        "ctx_delta_max_pct": segment["context_fans"]["setpoint_delta_max_pct"],
        "post_cpu_tctl_p90_c": segment["signals"]["post"]["cpu_tctl_c"]["p90"],
        "post_cpu_tctl_max_c": segment["signals"]["post"]["cpu_tctl_c"]["max"],
        "post_cpu_package_w_p90": segment["signals"]["post"]["cpu_package_w"]["p90"],
    }


SEGMENT_TRACE_FIELDS = [
    "segment_id", "rel_s", "wall_clock", "cpu_tctl_c", "cpu_package_w",
    "system_cpu_busy_pct", "gpu_memjn_c", "gpu_power_w",
    "rad_setpoint_avg_pct", "rad_duty_avg_pct", "rad_rpm_avg",
    "context_setpoint_avg_pct",
    "ch1_setpoint_pct", "ch1_duty_pct", "ch1_rpm", "ch1_response_source",
    "ch4_setpoint_pct", "ch4_duty_pct", "ch4_rpm", "ch4_response_source",
    "ch5_setpoint_pct", "ch5_duty_pct", "ch5_rpm", "ch5_response_source",
]


def write_segment_trace(path: Path, data: dict, segment: dict) -> None:
    records = data["records"]
    dt_s = data["tick_seconds"]
    event_index = segment["event"]["index"]
    indices = range(segment["window"]["start_index"],
                    segment["window"]["end_index"] + 1)
    rad_sp = _avg_channels(records, RADIATOR_CHANNELS, "setpoint_pct")
    rad_duty = _avg_channels(records, RADIATOR_CHANNELS, "duty_pct")
    rad_rpm = _avg_channels(records, RADIATOR_CHANNELS, "rpm")
    ctx_sp = _avg_channels(records, CONTEXT_CHANNELS, "setpoint_pct")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=SEGMENT_TRACE_FIELDS)
        writer.writeheader()
        for i in indices:
            row = {
                "segment_id": segment["segment_id"],
                "rel_s": round((i - event_index) * dt_s, 3),
                "wall_clock": records[i]["wall_clock"],
                "cpu_tctl_c": _round(data["cpu_tctl_c"][i]),
                "cpu_package_w": _round(data["cpu_package_w"][i]),
                "system_cpu_busy_pct": _round(data["system_cpu_busy_pct"][i]),
                "gpu_memjn_c": _round(data["gpu_memjn_c"][i]),
                "gpu_power_w": _round(data["gpu_power_w"][i]),
                "rad_setpoint_avg_pct": _round(rad_sp[i]),
                "rad_duty_avg_pct": _round(rad_duty[i]),
                "rad_rpm_avg": _round(rad_rpm[i]),
                "context_setpoint_avg_pct": _round(ctx_sp[i]),
            }
            for ch in RADIATOR_CHANNELS:
                row[f"ch{ch}_setpoint_pct"] = records[i].get(
                    f"channel{ch}_setpoint_pct", "")
                row[f"ch{ch}_duty_pct"] = records[i].get(
                    f"channel{ch}_duty_pct", "")
                row[f"ch{ch}_rpm"] = records[i].get(f"channel{ch}_rpm", "")
                row[f"ch{ch}_response_source"] = records[i].get(
                    f"channel{ch}_response_source", "")
            writer.writerow(row)


def write_outputs(out_dir: Path, summary: dict, data: dict,
                  write_segments: bool) -> dict[str, str]:
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    json_path = out_dir / f"cpu-aio-segments-{stamp}.json"
    csv_path = out_dir / f"cpu-aio-segments-{stamp}.csv"
    json_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    with csv_path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=SEGMENT_INDEX_FIELDS)
        writer.writeheader()
        for segment in summary["segments"]:
            writer.writerow(segment_index_row(segment))
    outputs = {"json": str(json_path), "csv": str(csv_path)}
    if write_segments:
        trace_dir = out_dir / f"cpu-aio-segments-{stamp}-traces"
        for segment in summary["segments"]:
            trace = trace_dir / f"segment-{segment['segment_id']:02d}.csv"
            write_segment_trace(trace, data, segment)
        outputs["segment_traces"] = str(trace_dir)
    return outputs


def analyze(csv_path: Path, *, cpu_tctl_thresholds_c: list[float],
            cpu_power_thresholds_w: list[float], pre_s: float, post_s: float,
            merge_events_within_s: float, gpu_power_confound_w: float,
            gpu_mem_confound_c: float, max_segments: int,
            stop_at_gpu_confound: bool = True) -> dict:
    data = load_csv(csv_path)
    clusters = find_event_clusters(data, cpu_tctl_thresholds_c,
                                   cpu_power_thresholds_w,
                                   merge_events_within_s)
    segments = []
    for segment_id, cluster in enumerate(clusters[:max_segments], start=1):
        indices = _indices_around(cluster["primary"]["index"],
                                  len(data["records"]), pre_s, post_s,
                                  data["tick_seconds"])
        segments.append(summarize_segment(data, cluster, segment_id, indices,
                                          gpu_power_confound_w,
                                          gpu_mem_confound_c,
                                          stop_at_gpu_confound))
    return {
        "csv": str(csv_path),
        "session": {k: data["meta"].get(k) for k in
                    ("session_start", "git_hash", "config_sha256", "mode")},
        "rows": len(data["records"]),
        "tick_seconds": data["tick_seconds"],
        "params": {
            "radiator_channels": list(RADIATOR_CHANNELS),
            "context_channels": list(CONTEXT_CHANNELS),
            "cpu_tctl_thresholds_c": cpu_tctl_thresholds_c,
            "cpu_power_thresholds_w": cpu_power_thresholds_w,
            "pre_s": pre_s,
            "post_s": post_s,
            "merge_events_within_s": merge_events_within_s,
            "gpu_power_confound_w": gpu_power_confound_w,
            "gpu_mem_confound_c": gpu_mem_confound_c,
            "max_segments": max_segments,
            "stop_at_gpu_confound": stop_at_gpu_confound,
        },
        "segments": segments,
        "_data": data,
    }


def _print_console(summary: dict, outputs: dict[str, str]) -> None:
    print(f"CPU/AIO segment extractor: {len(summary['segments'])} segment(s)")
    print(f"CSV: {summary['csv']}")
    for key, path in outputs.items():
        print(f"{key}: {path}")
    for segment in summary["segments"]:
        rad = segment["radiator_group"]
        ctx = segment["context"]
        print(
            f"segment {segment['segment_id']}: {segment['event']['wall_clock']} "
            f"{segment['event']['kind']} >= {segment['event']['threshold']} "
            f"CPU {ctx['event_cpu_tctl_c']} C / {ctx['event_cpu_package_w']} W, "
            f"rad +max {rad['setpoint_delta_max_pct']}%, "
            f"gpu_confounded={ctx['gpu_confounded']}"
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--csv", type=Path,
                        help="Control-loop CSV path. Defaults to latest runtime log.")
    parser.add_argument("--runtime-home", type=Path,
                        default=Path("release/runtime"),
                        help="Runtime home used to find latest CSV when --csv is omitted.")
    parser.add_argument("--out-dir", type=Path,
                        default=Path("release/runtime/analysis"),
                        help="Directory for JSON/CSV output.")
    parser.add_argument("--cpu-tctl-thresholds-c", default="64,72,75,84",
                        help="Comma-separated CPU Tctl crossing thresholds.")
    parser.add_argument("--cpu-power-thresholds-w", default="80,120,160",
                        help="Comma-separated CPU package-power crossing thresholds.")
    parser.add_argument("--pre-s", type=float, default=60.0,
                        help="Seconds before primary event to include.")
    parser.add_argument("--post-s", type=float, default=240.0,
                        help="Seconds after primary event to include.")
    parser.add_argument("--merge-events-within-s", type=float, default=60.0,
                        help="Cluster crossings into one segment within this span.")
    parser.add_argument("--gpu-power-confound-w", type=float, default=300.0,
                        help="GPU board power that marks a segment GPU-confounded.")
    parser.add_argument("--gpu-mem-confound-c", type=float, default=60.0,
                        help="GPU mem junction that marks a segment GPU-confounded.")
    parser.add_argument("--max-segments", type=int, default=20,
                        help="Maximum event clusters to summarize.")
    parser.add_argument("--no-segment-csv", action="store_true",
                        help="Skip per-segment compact row extracts.")
    parser.add_argument("--include-gpu-confounded-post", action="store_true",
                        help="Let summary metrics continue after GPU confound; "
                             "traces always include the full configured window.")
    args = parser.parse_args(argv)

    csv_path = args.csv if args.csv is not None else latest_control_csv(args.runtime_home)
    if not csv_path.is_file():
        print(f"CSV not found: {csv_path}", file=sys.stderr)
        return 2
    summary = analyze(
        csv_path,
        cpu_tctl_thresholds_c=_parse_thresholds(args.cpu_tctl_thresholds_c),
        cpu_power_thresholds_w=_parse_thresholds(args.cpu_power_thresholds_w),
        pre_s=args.pre_s,
        post_s=args.post_s,
        merge_events_within_s=args.merge_events_within_s,
        gpu_power_confound_w=args.gpu_power_confound_w,
        gpu_mem_confound_c=args.gpu_mem_confound_c,
        max_segments=args.max_segments,
        stop_at_gpu_confound=not args.include_gpu_confounded_post,
    )
    data = summary.pop("_data")
    outputs = write_outputs(args.out_dir, summary, data,
                            write_segments=not args.no_segment_csv)
    _print_console(summary, outputs)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
