#!/usr/bin/env python3
"""Phase-B replay analyzer for the transient power-anticipation ("spike catcher")
campaign (docs/cpu-transient-power-anticipation-plan-2026-06-15.md §8).

Companion to analyze_power_lead.py (lag scan) and analyze_power_onset.py (step
onset). Reads a spike-capture session (session.csv + manifest.json +
spike_markers.csv from `Capture-EnergySession.ps1 -SpikeLoad`) and, for each
injected burst, measures:

  (1) the PRIMARY, parameter-free number: the differential onset lead of the
      package-power slope over the free, validated dTctl/dt slope -- positive
      means power moved before the temperature derivative. This is the §4/§8
      contest (power-slope vs dTctl/dt, NOT vs busy%).
  (2) a replay of the §5 B^spk shadow term (slope band -> target-tracker
      integrator -> level gate -> decay-on-NaN), giving its per-burst onset and
      its p95 over the no-burst floor plateau (the §6/§8 no-disturbance check).

Honest about resolution: the shipped session.csv carries the MIRRORED ~1 s
energy window, so the power slope is coarse and a sub-second differential is not
resolvable until the un-mirrored producer change (plan §5.1/§9) logs
`cpu_pkg_watts_unmirrored`. The tool auto-detects that column and reports which
watts source and what time resolution it used.

Read-only and offline: stdlib only, no exe, no runtime home, no elevated rights.
It reports numbers; it does not decide the plan's go/no-go (§8/§10 own that).
"""
from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from control_csv import (  # noqa: E402
    WALL_CLOCK_FMT, column, parse_control_csv, to_float)
from analyze_power_lead import derive_watts, tick_seconds  # noqa: E402

UNMIRRORED_COL = "cpu_pkg_watts_unmirrored"


# ---- pure math (pinned by tests/test_power_spike.py) -----------------------

def ema(xs: list[float | None], dts: list[float], tau: float) -> list[float | None]:
    """First-order EMA using the measured per-row dt. A None input holds the
    last smoothed value (so the slope stays defined across a blanked row); the
    output is None until the first real sample arrives."""
    out: list[float | None] = []
    y: float | None = None
    for x, dt in zip(xs, dts):
        if x is None:
            out.append(y)
            continue
        if y is None:
            y = x
        else:
            a = 1.0 - math.exp(-dt / tau) if tau > 0 and dt > 0 else 1.0
            y = a * x + (1.0 - a) * y
        out.append(y)
    return out


def slope(xs: list[float | None], dts: list[float]) -> list[float | None]:
    """Backward difference per second; None where either endpoint is None."""
    out: list[float | None] = [None]
    for i in range(1, len(xs)):
        a, b = xs[i - 1], xs[i]
        if a is None or b is None or dts[i] <= 0:
            out.append(None)
        else:
            out.append((b - a) / dts[i])
    return out


def smootherstep(x: float) -> float:
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    return x * x * x * (x * (x * 6.0 - 15.0) + 10.0)


def slope_drive(s_tilde: float | None, w_tilde: float | None, *, s_start: float,
                s_full: float, w_floor: float, b_max: float) -> float:
    """§5.3/§5.4: smootherstep slope band, gated by the level floor. Undefined
    input -> 0 (the §5.6 decay-on-unavailable target)."""
    if s_tilde is None or w_tilde is None or s_full <= s_start:
        return 0.0
    g = 1.0 if w_tilde >= w_floor else 0.0
    u = (s_tilde - s_start) / (s_full - s_start)
    return g * smootherstep(u) * b_max


def integrate_target(drive: list[float], dts: list[float], rise: float,
                     fall: float) -> list[float]:
    """§5.5 slew-rate-limited target-tracker (a deliberate divergence from the
    shipped accumulator): slew B toward the target drive d_k, clamped to
    [-fall*dt, +rise*dt] per row."""
    out: list[float] = []
    b = 0.0
    for d, dt in zip(drive, dts):
        step = d - b
        lo, hi = -fall * dt, rise * dt
        if step < lo:
            step = lo
        elif step > hi:
            step = hi
        b += step
        out.append(b)
    return out


def onset_50pct(times: list[float], series: list[float | None], lo_i: int,
                hi_i: int) -> float | None:
    """Time (from times[lo_i]) at which `series` first reaches 50 % of its
    rise over the window [lo_i, hi_i]; threshold-light, like analyze_power_onset."""
    vals = [series[i] for i in range(lo_i, hi_i) if series[i] is not None]
    if len(vals) < 2:
        return None
    lo, hi = series[lo_i], max(vals)
    if lo is None:
        lo = vals[0]
    if hi <= lo:
        return None
    thr = lo + 0.5 * (hi - lo)
    for i in range(lo_i, hi_i):
        if series[i] is not None and series[i] >= thr:
            return times[i] - times[lo_i]
    return None


# ---- IO + alignment --------------------------------------------------------

def load_markers(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    out = []
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines:
        return []
    header = [c.strip() for c in lines[0].split(",")]
    for line in lines[1:]:
        if not line.strip():
            continue
        cells = line.split(",")
        row = {header[i]: cells[i].strip() for i in range(min(len(header), len(cells)))}
        out.append(row)
    return out


def watts_series(header, rows) -> tuple[list[float | None], str]:
    """Prefer the un-mirrored shadow column; else derive from the mirrored
    energy window. Returns (series, source-label)."""
    if UNMIRRORED_COL in header:
        return [to_float(v) for v in column(header, rows, UNMIRRORED_COL)], "unmirrored"
    return (derive_watts(
        column(header, rows, "cpu_power_sample_id"),
        column(header, rows, "cpu_pkg_energy_delta_uj"),
        column(header, rows, "cpu_power_window_ms"),
    ), "mirrored~1s")


def analyze(session_dir: Path, *, tau_w: float, tau_s: float, deriv_half_s: float,
            s_start: float, s_full: float, w_floor: float, b_max: float,
            rise: float, fall: float, post_s: float) -> dict:
    _, header, rows = parse_control_csv(session_dir / "session.csv")
    wall = column(header, rows, "wall_clock")
    dt_nom = tick_seconds(wall)
    n = len(rows)
    dts = [dt_nom] * n
    t = [i * dt_nom for i in range(n)]  # row-index time base (tick resolution)

    watts, wsrc = watts_series(header, rows)
    busy = [to_float(v) for v in column(header, rows, "system_cpu_busy_pct")]
    tctl = [to_float(v) for v in column(header, rows, "cpu_tctl_c")]

    w_t = ema(watts, dts, tau_w)
    s_raw = slope(w_t, dts)
    s_t = ema(s_raw, dts, tau_s)
    # dTctl/dt: central difference smoothed over +/- deriv_half_s, then EMA.
    c_t = ema(tctl, dts, tau_w)
    dtctl = slope(c_t, dts)
    dtctl_t = ema(dtctl, dts, tau_s)

    drive = [slope_drive(s_t[i], w_t[i], s_start=s_start, s_full=s_full,
                         w_floor=w_floor, b_max=b_max) for i in range(n)]
    bspk = integrate_target(drive, dts, rise, fall)

    # first session row index at or after a given wall_clock second
    first_at: dict[str, int] = {}
    for i, w in enumerate(wall):
        if w and w not in first_at:
            first_at[w] = i

    bursts = [m for m in load_markers(session_dir / "spike_markers.csv")
              if m.get("event") == "burst_start"]
    post_rows = max(2, int(post_s / dt_nom)) if dt_nom > 0 else 8
    per_burst = []
    for m in bursts:
        iso = m.get("iso", "")
        t0 = first_at.get(iso)
        if t0 is None:  # marker second not in the session (clock skew / gap)
            per_burst.append({"iso": iso, "aligned": False})
            continue
        hi = min(n, t0 + post_rows)
        pw = onset_50pct(t, s_t, t0, hi)
        dc = onset_50pct(t, dtctl_t, t0, hi)
        bk = onset_50pct(t, bspk, t0, hi)
        diff = (None if pw is None or dc is None else dc - pw)
        per_burst.append({
            "iso": iso, "aligned": True,
            "power_slope_onset_s": pw, "dtctl_onset_s": dc, "bspk_onset_s": bk,
            "differential_lead_s": diff,  # >0 => power slope leads dTctl/dt
        })

    diffs = [b["differential_lead_s"] for b in per_burst
             if b.get("differential_lead_s") is not None]

    # No-disturbance: B^spk over the steady floor window (manifest steady_*),
    # which -SpikeLoad sets to the clean initial floor before the first burst.
    floor_p95 = None
    try:
        ph = json.loads((session_dir / "manifest.json").read_text())["phases"]
        anchor = datetime.strptime(ph["load_start"], WALL_CLOCK_FMT)
        sa = (datetime.strptime(ph["steady_start"], WALL_CLOCK_FMT) - anchor).total_seconds()
        sb = (datetime.strptime(ph["steady_end"], WALL_CLOCK_FMT) - anchor).total_seconds()

        def rel(i):
            try:
                return (datetime.strptime(wall[i], WALL_CLOCK_FMT) - anchor).total_seconds()
            except ValueError:
                return None
        floor_vals = [bspk[i] for i in range(n)
                      if (r := rel(i)) is not None and sa <= r <= sb]
        if len(floor_vals) >= 20:
            floor_p95 = statistics.quantiles(floor_vals, n=20)[18]
        elif floor_vals:
            floor_p95 = max(floor_vals)
    except (KeyError, ValueError, FileNotFoundError):
        pass

    busy_vals = [b for b in busy if b is not None]
    return {
        "session_dir": str(session_dir),
        "rows": n, "tick_seconds": dt_nom, "watts_source": wsrc,
        "resolution_note": (
            "mirrored ~1 s energy window: power slope is coarse and a sub-second "
            "differential is not resolvable; capture cpu_pkg_watts_unmirrored "
            "(plan §5.1) for tick-resolution power onset"
            if wsrc != "unmirrored" else "un-mirrored tick-cadence watts"),
        "params": {"tau_w": tau_w, "tau_s": tau_s, "deriv_half_s": deriv_half_s,
                   "s_start": s_start, "s_full": s_full, "w_floor": w_floor,
                   "b_max": b_max, "rise": rise, "fall": fall, "post_s": post_s,
                   "note": "band/cap params are UN-TUNED placeholders; §8 fixes them from data"},
        "busy_pinned": ({"p50": statistics.median(busy_vals),
                         "min": min(busy_vals)} if busy_vals else None),
        "bursts": len(bursts),
        "per_burst": per_burst,
        "differential_lead_median_s": (statistics.median(diffs) if diffs else None),
        "differential_lead_n": len(diffs),
        "bspk_floor_p95": floor_p95,
    }


def render(r: dict) -> str:
    L = [
        "# Transient power-anticipation Phase-B replay",
        f"- session: `{r['session_dir']}` ({r['rows']} rows, ~{r['tick_seconds']:.3f} s/row)",
        f"- watts source: {r['watts_source']} -- {r['resolution_note']}",
        (f"- busy pinned: p50 {r['busy_pinned']['p50']:.1f} % / min "
         f"{r['busy_pinned']['min']:.1f} %" if r["busy_pinned"] else "- busy: n/a"),
        f"- bursts: {r['bursts']} ({r['differential_lead_n']} aligned + resolvable)",
        "",
        "## Primary: power-slope vs free dTctl/dt (differential onset lead)",
        (f"- median differential lead: {r['differential_lead_median_s']:+.2f} s "
         "(> 0 => power slope leads the temperature derivative)"
         if r["differential_lead_median_s"] is not None
         else "- median differential lead: n/a (no aligned/resolvable bursts)"),
        "",
        "## No-disturbance (B^spk over the steady floor)",
        (f"- B^spk floor p95: {r['bspk_floor_p95']:.3f} %% (want ~0)"
         if r["bspk_floor_p95"] is not None else "- B^spk floor p95: n/a"),
        "",
        "## Per-burst (seconds from burst marker)",
    ]
    for b in r["per_burst"]:
        if not b.get("aligned"):
            L.append(f"- {b['iso']}: NOT aligned to a session row (clock skew/gap)")
            continue
        def f(x):
            return "n/a" if x is None else f"{x:+.2f}s"
        L.append(f"- {b['iso']}: power {f(b['power_slope_onset_s'])}, "
                 f"dTctl {f(b['dtctl_onset_s'])}, B^spk {f(b['bspk_onset_s'])} "
                 f"=> diff {f(b['differential_lead_s'])}")
    L += ["", "Interpretation is owned by the plan §8/§10 (go/no-go: the power-slope "
          "lead must beat both fan-side latency and the free dTctl/dt by a useful margin)."]
    return "\n".join(L) + "\n"


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--session-dir", required=True, type=Path,
                   help="Spike-capture dir (session.csv + manifest.json + spike_markers.csv).")
    p.add_argument("--tau-w", type=float, default=0.5)
    p.add_argument("--tau-s", type=float, default=0.5)
    p.add_argument("--deriv-half-s", type=float, default=1.0)
    p.add_argument("--s-start", type=float, default=20.0, help="W/s (un-tuned).")
    p.add_argument("--s-full", type=float, default=120.0, help="W/s (un-tuned).")
    p.add_argument("--w-floor", type=float, default=100.0, help="W (clears the 94-121 W band).")
    p.add_argument("--b-max", type=float, default=10.0, help="%% (un-tuned).")
    p.add_argument("--rise", type=float, default=40.0, help="%%/s (un-tuned).")
    p.add_argument("--fall", type=float, default=8.0, help="%%/s (un-tuned).")
    p.add_argument("--post-s", type=float, default=20.0, help="onset search window after a burst.")
    p.add_argument("--format", choices=("markdown", "json"), default="markdown")
    p.add_argument("--out", type=Path)
    args = p.parse_args(argv)

    if not (args.session_dir / "session.csv").is_file():
        print(f"session.csv not found in {args.session_dir}", file=sys.stderr)
        return 2
    r = analyze(args.session_dir, tau_w=args.tau_w, tau_s=args.tau_s,
                deriv_half_s=args.deriv_half_s, s_start=args.s_start,
                s_full=args.s_full, w_floor=args.w_floor, b_max=args.b_max,
                rise=args.rise, fall=args.fall, post_s=args.post_s)
    text = json.dumps(r, indent=2) if args.format == "json" else render(r)
    if args.out:
        args.out.write_text(text, encoding="utf-8")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
