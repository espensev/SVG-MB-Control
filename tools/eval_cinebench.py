#!/usr/bin/env python3
"""Ad-hoc analysis of the latest control-loop CSV around the Cinebench window."""
from __future__ import annotations

import csv
import datetime as dt
import json
import statistics
import sys
from pathlib import Path

CSV_PATH = Path(
    r"D:\Development\Thermals\SVG-MB\SVG-MB-Control\release\runtime\logs\archive"
    r"\svg_mb_control_control-loop_20260524_024240.csv"
)
EVENTS_PATH = Path(
    r"D:\Development\Thermals\SVG-MB\SVG-MB-Control\release\runtime\logs"
    r"\svg_mb_control_events.jsonl"
)


def data_lines(path: Path):
    with path.open("r", encoding="utf-8", newline="") as fh:
        for line in fh:
            if not line.strip() or line.startswith("#"):
                continue
            yield line


def maybe_float(v):
    if v is None or v == "":
        return None
    try:
        f = float(v)
    except ValueError:
        return None
    return f


def parse_ts(s):
    if not s:
        return None
    try:
        return dt.datetime.fromisoformat(s)
    except ValueError:
        return None


def pct(arr, p):
    if not arr:
        return None
    k = (len(arr) - 1) * p
    f = int(k)
    c = min(f + 1, len(arr) - 1)
    arr_s = sorted(arr)
    if f == c:
        return arr_s[f]
    return arr_s[f] + (arr_s[c] - arr_s[f]) * (k - f)


def stats(arr):
    if not arr:
        return None
    arr_s = sorted(arr)
    return {
        "n": len(arr),
        "min": arr_s[0],
        "p50": pct(arr, 0.50),
        "p90": pct(arr, 0.90),
        "p95": pct(arr, 0.95),
        "p99": pct(arr, 0.99),
        "max": arr_s[-1],
        "mean": sum(arr) / len(arr),
    }


def fmt_stats(s, unit="", digits=2):
    if s is None:
        return "n=0"
    return (
        f"n={s['n']:>5} "
        f"min={s['min']:.{digits}f}{unit} "
        f"p50={s['p50']:.{digits}f}{unit} "
        f"p90={s['p90']:.{digits}f}{unit} "
        f"p95={s['p95']:.{digits}f}{unit} "
        f"max={s['max']:.{digits}f}{unit} "
        f"mean={s['mean']:.{digits}f}{unit}"
    )


def main():
    print(f"Loading {CSV_PATH.name}")
    with CSV_PATH.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(data_lines(CSV_PATH))
        rows = list(reader)
    print(f"  rows: {len(rows)}")

    # Extract timestamped rows
    parsed = []
    for r in rows:
        ts = parse_ts(r.get("wall_clock") or r.get("snapshot_time"))
        if ts is None:
            continue
        tctl = maybe_float(r.get("cpu_tctl_c"))
        memjn = maybe_float(r.get("gpu_memjn_c"))
        core = maybe_float(r.get("gpu_core_c"))
        parsed.append({
            "ts": ts,
            "tctl": tctl,
            "memjn": memjn,
            "core": core,
            "row": r,
        })

    if not parsed:
        print("no parseable rows")
        return 1

    t0 = parsed[0]["ts"]
    t1 = parsed[-1]["ts"]
    dur_s = (t1 - t0).total_seconds()
    print(f"  span: {t0} -> {t1} ({dur_s/60:.1f} min)")

    # Pre-scan: where does Tctl exceed 80C? Cinebench multi-core will pin Tctl high
    # for ~10 min with relatively flat readings.
    hot_threshold = 80.0
    print(f"\n== Searching for Cinebench-like Tctl windows (>={hot_threshold} C) ==")

    # Find contiguous runs of hot rows (allowing 5 s gaps for short dips)
    hot_rows = [(i, p) for i, p in enumerate(parsed) if p["tctl"] is not None and p["tctl"] >= hot_threshold]
    print(f"  rows above {hot_threshold} C: {len(hot_rows)}")

    if not hot_rows:
        print("no Cinebench-like window found")
        return 1

    # Group into windows: split where gap > 30 s
    windows = []
    cur = [hot_rows[0]]
    for prev, nxt in zip(hot_rows, hot_rows[1:]):
        gap = (nxt[1]["ts"] - prev[1]["ts"]).total_seconds()
        if gap > 30:
            windows.append(cur)
            cur = []
        cur.append(nxt)
    if cur:
        windows.append(cur)

    print(f"  windows found: {len(windows)}")
    for w in windows:
        ws, we = w[0][1]["ts"], w[-1][1]["ts"]
        dur = (we - ws).total_seconds()
        tctls = [p[1]["tctl"] for p in w if p[1]["tctl"] is not None]
        max_t = max(tctls) if tctls else None
        mean_t = sum(tctls)/len(tctls) if tctls else None
        print(f"    {ws}  ->  {we}   dur={dur:6.1f}s   max={max_t:.2f}C  mean={mean_t:.2f}C   n={len(w)}")

    # Pick the largest window with mean >= 82C (typical Cinebench multi-core sustain)
    candidate = max(
        (w for w in windows if w and (sum((p[1]["tctl"] or 0) for p in w)/len(w)) >= 82.0),
        key=lambda w: (w[-1][1]["ts"] - w[0][1]["ts"]).total_seconds(),
        default=None,
    )
    if candidate is None:
        candidate = max(windows, key=lambda w: (w[-1][1]["ts"] - w[0][1]["ts"]).total_seconds())
    win_start_i = candidate[0][0]
    win_end_i = candidate[-1][0]

    # Include 60 s pre-roll and 5 min post-roll for cooldown
    pre_i = win_start_i
    while pre_i > 0 and (parsed[win_start_i]["ts"] - parsed[pre_i - 1]["ts"]).total_seconds() < 60:
        pre_i -= 1
    post_i = win_end_i
    while post_i < len(parsed) - 1 and (parsed[post_i + 1]["ts"] - parsed[win_end_i]["ts"]).total_seconds() < 300:
        post_i += 1

    win_rows = parsed[win_start_i:win_end_i + 1]
    pre_rows = parsed[pre_i:win_start_i]
    post_rows = parsed[win_end_i + 1:post_i + 1]

    print(f"\n== Selected window ==")
    print(f"  pre   : {parsed[pre_i]['ts']} ({len(pre_rows)} rows, {(parsed[win_start_i]['ts']-parsed[pre_i]['ts']).total_seconds():.1f}s)")
    print(f"  load  : {parsed[win_start_i]['ts']} -> {parsed[win_end_i]['ts']} ({len(win_rows)} rows, {(parsed[win_end_i]['ts']-parsed[win_start_i]['ts']).total_seconds():.1f}s)")
    print(f"  post  : {parsed[win_end_i+1]['ts'] if post_rows else '-'} ({len(post_rows)} rows, {(parsed[post_i]['ts']-parsed[win_end_i]['ts']).total_seconds():.1f}s)")

    def col(rows, key, transform=None):
        vals = []
        for p in rows:
            v = maybe_float(p["row"].get(key))
            if v is None:
                continue
            if transform:
                v = transform(v)
            vals.append(v)
        return vals

    # Pre/load/post stats
    for label, rs in (("PRE", pre_rows), ("LOAD", win_rows), ("POST", post_rows)):
        print(f"\n-- {label} --")
        print(f"  CPU Tctl     : {fmt_stats(stats(col(rs, 'cpu_tctl_c')), ' C')}")
        print(f"  GPU memjn    : {fmt_stats(stats(col(rs, 'gpu_memjn_c')), ' C')}")
        print(f"  GPU core     : {fmt_stats(stats(col(rs, 'gpu_core_c')), ' C')}")
        for ch in range(7):
            duty = col(rs, f"fan{ch}_duty_pct")
            rpm  = col(rs, f"fan{ch}_rpm")
            sp   = col(rs, f"channel{ch}_setpoint_pct")
            mode = col(rs, f"fan{ch}_mode_raw")
            mode_zero = sum(1 for m in mode if m == 0) if mode else 0
            mode_str = f"mode0={mode_zero}/{len(mode)}" if mode else "mode=?"
            sp_s = stats(sp) if sp else None
            duty_s = stats(duty) if duty else None
            rpm_s = stats(rpm) if rpm else None
            print(f"  ch{ch}: setpoint p50={sp_s['p50']:.1f}% p90={sp_s['p90']:.1f}% max={sp_s['max']:.1f}%" if sp_s else f"  ch{ch}: setpoint -")
            print(f"        duty     p50={duty_s['p50']:.1f}% p90={duty_s['p90']:.1f}% max={duty_s['max']:.1f}%   {mode_str}" if duty_s else "")
            print(f"        rpm      p50={rpm_s['p50']:.0f}  p90={rpm_s['p90']:.0f}  max={rpm_s['max']:.0f}" if rpm_s else "")

    # Compute time-to-first-duty-increase: how long after Cinebench window start
    # before each channel commands a setpoint above its PRE p95 baseline?
    print("\n== Time-to-first-step (vs PRE p95 baseline + 1.5%) ==")
    for ch in range(6):
        sp_pre = col(pre_rows, f"channel{ch}_setpoint_pct")
        if not sp_pre:
            continue
        base = pct(sp_pre, 0.95) + 1.5
        t_load_start = parsed[win_start_i]["ts"]
        t_step = None
        for p in win_rows:
            v = maybe_float(p["row"].get(f"channel{ch}_setpoint_pct"))
            if v is None:
                continue
            if v >= base:
                t_step = (p["ts"] - t_load_start).total_seconds()
                break
        print(f"  ch{ch}: PRE p95 baseline = {pct(sp_pre, 0.95):.2f}% -> threshold {base:.2f}%   first crossing: {t_step}s")

    # Cooldown: time from load end until CPU Tctl drops below 70 C
    print("\n== Cooldown ==")
    t_load_end = parsed[win_end_i]["ts"]
    t_cool_70 = None
    t_cool_65 = None
    for p in post_rows:
        if p["tctl"] is None:
            continue
        if t_cool_70 is None and p["tctl"] < 70.0:
            t_cool_70 = (p["ts"] - t_load_end).total_seconds()
        if t_cool_65 is None and p["tctl"] < 65.0:
            t_cool_65 = (p["ts"] - t_load_end).total_seconds()
            break
    print(f"  CPU Tctl < 70 C : {t_cool_70} s after load end")
    print(f"  CPU Tctl < 65 C : {t_cool_65} s after load end")

    # Authority/reassert events during load
    if EVENTS_PATH.exists():
        load_start = parsed[win_start_i]["ts"]
        load_end = parsed[win_end_i]["ts"]
        evs = []
        with EVENTS_PATH.open("r", encoding="utf-8") as fh:
            for line in fh:
                try:
                    e = json.loads(line)
                except json.JSONDecodeError:
                    continue
                ts = parse_ts(e.get("ts") or e.get("wall_clock") or e.get("time"))
                if ts is None:
                    continue
                if load_start <= ts <= load_end:
                    evs.append(e)
        print(f"\n== Events during load: {len(evs)} ==")
        evt_kinds = {}
        for e in evs:
            k = e.get("event") or e.get("kind") or e.get("type") or "?"
            evt_kinds[k] = evt_kinds.get(k, 0) + 1
        for k, v in sorted(evt_kinds.items(), key=lambda x: -x[1])[:20]:
            print(f"  {v:>5}  {k}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
