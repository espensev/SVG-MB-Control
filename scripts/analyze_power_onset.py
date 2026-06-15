#!/usr/bin/env python3
"""Event-onset timing for the proposed power-anticipation boost (Gate 2(a)).

Companion to scripts/analyze_power_lead.py. Where that tool reports a
lag-scanned Pearson correlation between signal levels, this tool measures the
time at which each signal (derived package watts, system_cpu_busy_pct, and
cpu_tctl_c) first crosses a fixed fraction of its own idle->steady span,
relative to the manifest `load_start`. Crossing times are differenced to give
the inter-signal onset offsets.

Each signal is crossed at `span_fraction` of its own idle->steady amplitude
(idle = a low quantile of the pre-load window, robust to background activity on
a live machine; steady = the median over the manifest steady window). Using a
per-signal relative threshold makes the crossing time independent of each
signal's absolute baseline and units.

Read-only and offline: parses the CSV with the stdlib only, needs no exe, no
runtime home, and no elevated rights. Requires an energy-enabled session
directory containing manifest.json (for the phase boundaries) and session.csv.

This script reports numbers. It does not interpret them against the feed-forward
plan's go/no-go criteria (docs/cpu-power-feedforward-plan-2026-06-10.md Section 5/6).
"""
from __future__ import annotations

import argparse
import csv as csvmod
import json
import statistics
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from control_csv import (  # noqa: E402
    WALL_CLOCK_FMT, column, parse_control_csv, to_float)
from analyze_power_lead import derive_watts, tick_seconds  # noqa: E402

NAMED_TCTL_THRESHOLDS_C = (64.0, 84.0)  # midband_pressure_start_c, lowest
#                                         thermal_pressure_start_c in control.json


def _secs(wall_clock: str, anchor: datetime) -> float | None:
    try:
        return (datetime.strptime(wall_clock, WALL_CLOCK_FMT) - anchor).total_seconds()
    except ValueError:
        return None


def _quantile(values: list[float], q: float) -> float | None:
    xs = sorted(v for v in values if v is not None)
    if not xs:
        return None
    if len(xs) < 20:
        return xs[max(0, min(len(xs) - 1, round(q * (len(xs) - 1))))]
    return statistics.quantiles(xs, n=100)[max(0, min(98, round(q * 100) - 1))]


def _first_sustained(times: list[float | None], values: list[float | None],
                     threshold: float, sustain_rows: int) -> float | None:
    """First time at which values stay strictly above threshold for at least
    sustain_rows consecutive rows; returns the time of the first row of that run."""
    run = 0
    run_start: float | None = None
    for t, v in zip(times, values):
        if v is not None and t is not None and v > threshold:
            if run == 0:
                run_start = t
            run += 1
            if run >= sustain_rows:
                return run_start
        else:
            run = 0
            run_start = None
    return None


def _levels(times: list[float | None], values: list[float | None],
            pre_lo: float, pre_hi: float, steady_a: float, steady_b: float,
            idle_quantile: float, span_fraction: float) -> dict:
    idle_vals = [v for t, v in zip(times, values)
                 if t is not None and pre_lo <= t <= pre_hi]
    steady_vals = [v for t, v in zip(times, values)
                   if t is not None and steady_a <= t <= steady_b and v is not None]
    idle = _quantile(idle_vals, idle_quantile)
    steady = statistics.median(steady_vals) if steady_vals else None
    threshold = (idle + span_fraction * (steady - idle)
                 if idle is not None and steady is not None and steady > idle
                 else None)
    return {"idle": idle, "steady": steady, "threshold": threshold}


TRACE_FIELDS = ["t_rel_load_start_s", "wall_clock", "cpu_pkg_watts",
                "system_cpu_busy_pct", "cpu_tctl_c", "cpu_power_sample_id"]


def _write_trace(path: Path, wall, times, watts, busy, tctl, sample_id) -> None:
    """Tidy per-row signal extract: the columns the onset method reads, lifted
    out of the 100+-column session.csv, with time relative to load_start."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        w = csvmod.writer(fh)
        w.writerow(TRACE_FIELDS)
        for wc, t, pw, b, c, sid in zip(wall, times, watts, busy, tctl, sample_id):
            w.writerow([
                "" if t is None else round(t, 3), wc,
                "" if pw is None else round(pw, 4),
                "" if b is None else b, "" if c is None else c, sid,
            ])


def analyze(session_dir: Path, span_fraction: float, idle_quantile: float,
            pre_load_window_s: float, onset_window_pre_s: float,
            onset_window_post_s: float, sustain_s: float,
            trace_path: Path | None = None) -> dict:
    manifest = json.loads((session_dir / "manifest.json").read_text())
    phases = manifest["phases"]
    anchor = datetime.strptime(phases["load_start"], WALL_CLOCK_FMT)

    def rel(key: str) -> float:
        return (datetime.strptime(phases[key], WALL_CLOCK_FMT) - anchor).total_seconds()

    steady_a, steady_b = rel("steady_start"), rel("steady_end")
    meta, header, rows = parse_control_csv(session_dir / "session.csv")
    wall = column(header, rows, "wall_clock")
    dt_s = tick_seconds(wall)
    times = [_secs(w, anchor) for w in wall]
    tctl = [to_float(v) for v in column(header, rows, "cpu_tctl_c")]
    busy = [to_float(v) for v in column(header, rows, "system_cpu_busy_pct")]
    sample_id = column(header, rows, "cpu_power_sample_id")
    watts = derive_watts(
        sample_id,
        column(header, rows, "cpu_pkg_energy_delta_uj"),
        column(header, rows, "cpu_power_window_ms"),
    )
    if trace_path is not None:
        _write_trace(trace_path, wall, times, watts, busy, tctl, sample_id)

    sustain_rows = max(3, round(sustain_s / dt_s)) if dt_s > 0 else 3
    series = {"watts": watts, "busy_pct": busy, "tctl_c": tctl}
    levels = {name: _levels(times, vals, -pre_load_window_s, -10.0,
                            steady_a, steady_b, idle_quantile, span_fraction)
              for name, vals in series.items()}

    # crossings restricted to the onset window around load_start
    win_lo, win_hi = -onset_window_pre_s, onset_window_post_s
    win_idx = [i for i, t in enumerate(times)
               if t is not None and win_lo <= t <= win_hi]
    wt = [times[i] for i in win_idx]
    onset = {}
    for name, vals in series.items():
        thr = levels[name]["threshold"]
        wv = [vals[i] for i in win_idx]
        onset[name] = (_first_sustained(wt, wv, thr, sustain_rows)
                       if thr is not None else None)

    def delta(a: str, b: str) -> float | None:
        return (None if onset[a] is None or onset[b] is None
                else onset[a] - onset[b])

    tctl_vals = [v for v in tctl if v is not None]
    named = {}
    for thr in NAMED_TCTL_THRESHOLDS_C:
        named[f"cross_{int(thr)}c_s"] = _first_sustained(times, tctl, thr, sustain_rows)

    return {
        "session_dir": str(session_dir),
        "session": {
            "label": manifest.get("session_label"),
            "session_start": meta.get("session_start"),
            "git_hash": meta.get("git_hash"),
            "config_sha256": meta.get("config_sha256"),
            "load_threads": manifest.get("load_threads"),
        },
        "phases_rel_load_start_s": {
            "idle_start": rel("idle_start"), "load_start": 0.0,
            "steady_start": steady_a, "steady_end": steady_b,
            "cooldown_start": rel("cooldown_start"),
        },
        "rows": len(rows),
        "tick_seconds": dt_s,
        "params": {
            "span_fraction": span_fraction,
            "idle_quantile": idle_quantile,
            "pre_load_window_s": pre_load_window_s,
            "onset_window_pre_s": onset_window_pre_s,
            "onset_window_post_s": onset_window_post_s,
            "sustain_s": sustain_s,
            "sustain_rows": sustain_rows,
        },
        "levels": levels,
        "onset_s": onset,
        "deltas_s": {
            "watts_minus_busy": delta("watts", "busy_pct"),
            "tctl_minus_watts": delta("tctl_c", "watts"),
            "tctl_minus_busy": delta("tctl_c", "busy_pct"),
        },
        "tctl": {
            "max_c": max(tctl_vals) if tctl_vals else None,
            **named,
        },
    }


CSV_FIELDS = [
    "session_label", "session_start", "git_hash", "load_threads", "rows",
    "tick_seconds", "watts_idle", "watts_steady", "watts_threshold",
    "busy_idle", "busy_steady", "busy_threshold", "tctl_idle", "tctl_steady",
    "tctl_threshold", "onset_watts_s", "onset_busy_s", "onset_tctl_s",
    "watts_minus_busy_s", "tctl_minus_watts_s", "tctl_minus_busy_s",
    "tctl_max_c", "tctl_cross_64c_s", "tctl_cross_84c_s",
]


def csv_row(r: dict) -> dict:
    lv = r["levels"]
    return {
        "session_label": r["session"]["label"],
        "session_start": r["session"]["session_start"],
        "git_hash": r["session"]["git_hash"],
        "load_threads": r["session"]["load_threads"],
        "rows": r["rows"], "tick_seconds": round(r["tick_seconds"], 4),
        "watts_idle": lv["watts"]["idle"], "watts_steady": lv["watts"]["steady"],
        "watts_threshold": lv["watts"]["threshold"],
        "busy_idle": lv["busy_pct"]["idle"], "busy_steady": lv["busy_pct"]["steady"],
        "busy_threshold": lv["busy_pct"]["threshold"],
        "tctl_idle": lv["tctl_c"]["idle"], "tctl_steady": lv["tctl_c"]["steady"],
        "tctl_threshold": lv["tctl_c"]["threshold"],
        "onset_watts_s": r["onset_s"]["watts"], "onset_busy_s": r["onset_s"]["busy_pct"],
        "onset_tctl_s": r["onset_s"]["tctl_c"],
        "watts_minus_busy_s": r["deltas_s"]["watts_minus_busy"],
        "tctl_minus_watts_s": r["deltas_s"]["tctl_minus_watts"],
        "tctl_minus_busy_s": r["deltas_s"]["tctl_minus_busy"],
        "tctl_max_c": r["tctl"]["max_c"], "tctl_cross_64c_s": r["tctl"]["cross_64c_s"],
        "tctl_cross_84c_s": r["tctl"]["cross_84c_s"],
    }


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--session-dir", required=True, type=Path, action="append",
                   help="Session folder (manifest.json + session.csv). Repeatable.")
    p.add_argument("--span-fraction", type=float, default=0.5)
    p.add_argument("--idle-quantile", type=float, default=0.10)
    p.add_argument("--pre-load-window-s", type=float, default=300.0)
    p.add_argument("--onset-window-pre-s", type=float, default=60.0)
    p.add_argument("--onset-window-post-s", type=float, default=150.0)
    p.add_argument("--sustain-s", type=float, default=2.0)
    p.add_argument("--json-dir", type=Path, help="Write per-session JSON here.")
    p.add_argument("--csv-out", type=Path, help="Write the combined CSV here.")
    p.add_argument("--trace-dir", type=Path,
                   help="Write a tidy per-row signal extract (trace_<dir>.csv) "
                        "per session here.")
    args = p.parse_args(argv)

    results = []
    for d in args.session_dir:
        if not (d / "manifest.json").is_file() or not (d / "session.csv").is_file():
            print(f"missing manifest.json or session.csv in {d}", file=sys.stderr)
            return 2
        trace_path = (args.trace_dir / f"trace_{d.name}.csv"
                      if args.trace_dir else None)
        r = analyze(d, args.span_fraction, args.idle_quantile,
                    args.pre_load_window_s, args.onset_window_pre_s,
                    args.onset_window_post_s, args.sustain_s, trace_path)
        results.append(r)
        if args.json_dir:
            args.json_dir.mkdir(parents=True, exist_ok=True)
            label = (r["session"]["label"] or d.name).replace(" ", "_")
            (args.json_dir / f"onset_{label}.json").write_text(
                json.dumps(r, indent=2), encoding="utf-8")

    if args.csv_out:
        args.csv_out.parent.mkdir(parents=True, exist_ok=True)
        with args.csv_out.open("w", newline="", encoding="utf-8") as fh:
            w = csvmod.DictWriter(fh, fieldnames=CSV_FIELDS)
            w.writeheader()
            for r in results:
                w.writerow(csv_row(r))

    if not args.json_dir and not args.csv_out:
        print(json.dumps(results, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
