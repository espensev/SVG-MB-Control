#!/usr/bin/env python3
"""Gate 2 characterization for the proposed power-anticipation boost.

Measures, from a control-loop CSV, the evidence the feed-forward plan
(docs/cpu-power-feedforward-plan-2026-06-10.md §5 Gate 2) requires before any
FEAT intake:

  (a) power->Tctl lead time on load steps (lag scan of Pearson correlation
      against Tctl and against a smoothed dTctl/dt),
  (b) the idle-floor watts distribution and window noise (sets a lower bound
      for any `start_w`),
  (c) energy-window coverage (distinct windows, blanked windows, acquisition
      marker mix),
  (d) the same lead analysis for `system_cpu_busy_pct` -- the already-validated
      alternative signal the plan requires comparing against.

Read-only and offline: parses the CSV with the stdlib only, needs no exe, no
runtime home, and no elevated rights. The power legs need an energy-enabled
session (see docs/cpu-energy-quarantine-exit-capture-runbook-2026-06-10.md);
the busy-pct legs work on any session, including current disabled ones.

This script reports numbers; it does not decide the plan's go/no-go.
"""
from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from datetime import datetime
from pathlib import Path

# scripts/ is not a package; make the shared helpers importable under the
# by-file-path loaders too (tests, scheduled tasks).
sys.path.insert(0, str(Path(__file__).resolve().parent))
from control_csv import (  # noqa: E402
    WALL_CLOCK_FMT, column, parse_control_csv, to_float)

DEFAULT_TICK_S = 0.25  # shipped poll_tick_ms when the span cannot be derived

REQUIRED_COLUMNS = ("wall_clock", "cpu_tctl_c")


def derive_watts(sample_ids: list[str], deltas_uj: list[str],
                 windows_ms: list[str]) -> list[float | None]:
    """Per-row package watts. Rows mirror their resource window, so the value
    repeats across the ticks of one window; blanked/absent windows are None."""
    out: list[float | None] = []
    for sid, delta, window in zip(sample_ids, deltas_uj, windows_ms):
        if not sid or sid == "0":
            out.append(None)
            continue
        d = to_float(delta)
        w = to_float(window)
        if d is None or w is None or w <= 0.0 or d < 0.0:
            out.append(None)
            continue
        out.append(d / w / 1000.0)
    return out


def tick_seconds(wall_clocks: list[str]) -> float:
    """Nominal seconds per row from the wall-clock span (second-resolution
    timestamps over thousands of 250 ms rows)."""
    if len(wall_clocks) < 2:
        return DEFAULT_TICK_S
    try:
        first = datetime.strptime(wall_clocks[0], WALL_CLOCK_FMT)
        last = datetime.strptime(wall_clocks[-1], WALL_CLOCK_FMT)
    except ValueError:
        return DEFAULT_TICK_S
    span = (last - first).total_seconds()
    if span <= 0.0:
        return DEFAULT_TICK_S
    return span / (len(wall_clocks) - 1)


def pearson(xs: list[float | None], ys: list[float | None],
            min_pairs: int = 16) -> float | None:
    pairs = [(x, y) for x, y in zip(xs, ys) if x is not None and y is not None]
    if len(pairs) < min_pairs:
        return None
    px = [p[0] for p in pairs]
    py = [p[1] for p in pairs]
    mx = statistics.fmean(px)
    my = statistics.fmean(py)
    sxx = sum((x - mx) ** 2 for x in px)
    syy = sum((y - my) ** 2 for y in py)
    if sxx <= 0.0 or syy <= 0.0:
        return None
    sxy = sum((x - mx) * (y - my) for x, y in pairs)
    return sxy / math.sqrt(sxx * syy)


def best_lead(driver: list[float | None], response: list[float | None],
              dt_s: float, max_lag_s: float) -> dict | None:
    """Scan non-negative lags (driver earlier than response) and return the lag
    with the highest positive correlation: {'lead_s', 'r', 'r_at_zero'}."""
    n = len(driver)
    max_lag_ticks = int(max_lag_s / dt_s) if dt_s > 0 else 0
    best: tuple[float, int] | None = None
    r_at_zero = pearson(driver, response)
    for lag in range(0, max_lag_ticks + 1):
        if n - lag < 2:
            break
        r = pearson(driver[: n - lag] if lag else driver, response[lag:])
        if r is None:
            continue
        if best is None or r > best[0]:
            best = (r, lag)
    if best is None:
        return None
    return {"lead_s": best[1] * dt_s, "r": best[0], "r_at_zero": r_at_zero}


def smoothed_derivative(values: list[float | None], dt_s: float,
                        half_window_s: float) -> list[float | None]:
    """Central-difference derivative over +/- half_window_s (units per second)."""
    w = max(1, int(half_window_s / dt_s)) if dt_s > 0 else 1
    out: list[float | None] = [None] * len(values)
    for i in range(w, len(values) - w):
        lo = values[i - w]
        hi = values[i + w]
        if lo is None or hi is None:
            continue
        out[i] = (hi - lo) / (2 * w * dt_s)
    return out


def series_stats(values: list[float | None]) -> dict | None:
    xs = [v for v in values if v is not None]
    if not xs:
        return None
    out = {
        "count": len(xs),
        "min": min(xs),
        "p50": statistics.median(xs),
        "max": max(xs),
        "stdev": statistics.stdev(xs) if len(xs) >= 2 else 0.0,
    }
    out["p95"] = (statistics.quantiles(xs, n=20)[18]
                  if len(xs) >= 20 else max(xs))
    return out


def split_by_busy(values: list[float | None], busy: list[float | None],
                  idle_below_pct: float) -> tuple[list[float | None], list[float | None]]:
    idle: list[float | None] = []
    load: list[float | None] = []
    for v, b in zip(values, busy):
        if b is None:
            continue
        (idle if b < idle_below_pct else load).append(v)
    return idle, load


def window_coverage(sample_ids: list[str], watts: list[float | None],
                    acquisition: list[str]) -> dict:
    distinct: set[str] = set()
    blank: set[str] = set()
    for sid, w in zip(sample_ids, watts):
        if not sid or sid == "0":
            continue
        distinct.add(sid)
        if w is None:
            blank.add(sid)
    markers: dict[str, int] = {}
    for m in acquisition:
        key = m if m else "(blank)"
        markers[key] = markers.get(key, 0) + 1
    return {
        "distinct_windows": len(distinct),
        "blanked_windows": len(blank),
        "acquisition_rows": markers,
    }


def analyze(path: Path, max_lag_s: float, idle_busy_pct: float,
            deriv_half_window_s: float) -> dict:
    meta, header, rows = parse_control_csv(path)
    missing = [c for c in REQUIRED_COLUMNS if c not in header]
    if missing:
        raise SystemExit(f"CSV is missing required column(s): {missing}")

    wall = column(header, rows, "wall_clock")
    dt_s = tick_seconds(wall)
    tctl = [to_float(v) for v in column(header, rows, "cpu_tctl_c")]
    busy = [to_float(v) for v in column(header, rows, "system_cpu_busy_pct")]
    watts = derive_watts(
        column(header, rows, "cpu_power_sample_id"),
        column(header, rows, "cpu_pkg_energy_delta_uj"),
        column(header, rows, "cpu_power_window_ms"),
    )
    dtctl = smoothed_derivative(tctl, dt_s, deriv_half_window_s)

    idle_watts, load_watts = split_by_busy(watts, busy, idle_busy_pct)
    coverage = window_coverage(
        column(header, rows, "cpu_power_sample_id"), watts,
        column(header, rows, "cpu_pkg_energy_acquisition"))

    result = {
        "csv": str(path),
        "session": {k: meta.get(k) for k in
                    ("session_start", "git_hash", "config_sha256", "mode")},
        "rows": len(rows),
        "tick_seconds": dt_s,
        "max_lag_s": max_lag_s,
        "idle_busy_pct": idle_busy_pct,
        "coverage": coverage,
        "watts_idle": series_stats(idle_watts),
        "watts_load": series_stats(load_watts),
        "lead": {
            "watts_to_tctl": best_lead(watts, tctl, dt_s, max_lag_s),
            "watts_to_dtctl": best_lead(watts, dtctl, dt_s, max_lag_s),
            "busy_to_tctl": best_lead(busy, tctl, dt_s, max_lag_s),
            "busy_to_dtctl": best_lead(busy, dtctl, dt_s, max_lag_s),
        },
    }
    return result


def fmt_lead(entry: dict | None) -> str:
    if entry is None:
        return "n/a (insufficient overlapping data)"
    return (f"lead {entry['lead_s']:.2f} s (r={entry['r']:.3f}, "
            f"r at lag 0 = "
            f"{'n/a' if entry['r_at_zero'] is None else format(entry['r_at_zero'], '.3f')})")


def fmt_stats(entry: dict | None, unit: str) -> str:
    if entry is None:
        return "n/a (no values)"
    return (f"n={entry['count']}  min {entry['min']:.1f}  p50 {entry['p50']:.1f}"
            f"  p95 {entry['p95']:.1f}  max {entry['max']:.1f}"
            f"  stdev {entry['stdev']:.2f} {unit}")


def render_markdown(r: dict) -> str:
    s = r["session"]
    lines = [
        "# Power-anticipation Gate 2 characterization",
        "",
        f"- CSV: `{r['csv']}` ({r['rows']} rows, ~{r['tick_seconds']:.3f} s/row)",
        f"- Session: start {s.get('session_start')}, git `{s.get('git_hash')}`,"
        f" config `{(s.get('config_sha256') or '')[:12]}`",
        f"- Energy windows: {r['coverage']['distinct_windows']} distinct,"
        f" {r['coverage']['blanked_windows']} blanked;"
        f" acquisition rows {r['coverage']['acquisition_rows']}",
        "",
        "## Watts distributions (start_w must clear the idle band)",
        f"- idle (busy < {r['idle_busy_pct']:.0f} %): "
        + fmt_stats(r["watts_idle"], "W"),
        f"- load (busy >= {r['idle_busy_pct']:.0f} %): "
        + fmt_stats(r["watts_load"], "W"),
        "",
        f"## Lead-time scan (0..{r['max_lag_s']:.0f} s, driver earlier)",
        f"- package watts -> cpu_tctl_c:    {fmt_lead(r['lead']['watts_to_tctl'])}",
        f"- package watts -> dTctl/dt:      {fmt_lead(r['lead']['watts_to_dtctl'])}",
        f"- system busy %  -> cpu_tctl_c:   {fmt_lead(r['lead']['busy_to_tctl'])}",
        f"- system busy %  -> dTctl/dt:     {fmt_lead(r['lead']['busy_to_dtctl'])}",
        "",
        "Interpretation is owned by the plan's Gate 2"
        " (docs/cpu-power-feedforward-plan-2026-06-10.md §5): a power lead that"
        " does not usefully exceed fan-side response latency, or a busy-pct lead"
        " close to the power lead, are both documented no-go outcomes.",
    ]
    if r["coverage"]["distinct_windows"] == 0:
        lines.insert(4, "")
        lines.insert(5, "> No energy windows in this CSV (energy mode disabled)."
                        " Power legs are n/a; busy-pct legs above are still valid."
                        " Capture an enabled session per"
                        " docs/cpu-energy-quarantine-exit-capture-runbook-2026-06-10.md.")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--csv", required=True, type=Path,
                        help="Control-loop CSV path.")
    parser.add_argument("--max-lag-s", type=float, default=30.0,
                        help="Largest lead (seconds) to scan (default 30).")
    parser.add_argument("--idle-busy-pct", type=float, default=5.0,
                        help="system_cpu_busy_pct below this is idle (default 5).")
    parser.add_argument("--deriv-half-window-s", type=float, default=1.0,
                        help="Half-window for the smoothed dTctl/dt (default 1 s).")
    parser.add_argument("--format", choices=("markdown", "json"),
                        default="markdown")
    parser.add_argument("--out", type=Path, help="Write the report here.")
    args = parser.parse_args(argv)

    if not args.csv.is_file():
        print(f"CSV not found: {args.csv}", file=sys.stderr)
        return 2
    result = analyze(args.csv, args.max_lag_s, args.idle_busy_pct,
                     args.deriv_half_window_s)
    text = (json.dumps(result, indent=2) if args.format == "json"
            else render_markdown(result))
    if args.out:
        args.out.write_text(text, encoding="utf-8")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
