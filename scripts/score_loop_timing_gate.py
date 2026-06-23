#!/usr/bin/env python3
"""FEAT-0006 section-12 loop-timing gate for the all-core off-thread sweeper.

Read-only. Compares `loop_work_duration_ms` between a cycles-OFF baseline capture
and a candidate capture (the FEAT-0020 gate-6 analog) to answer one question
before any enabled-live all-core evidence is promoted out of quarantine: does the
off-thread sweeper move the shipped 250 ms control-loop profile?

Why this metric, why bucketed:
  * `loop_work_duration_ms` (NOT `loop_slip_ms`): slip is masked by the loop's
    fixed-start sleep, so it understates added work. score_energy_session.py
    criterion-6 deliberately uses slip for the IN-LINE energy read; this gate is
    a SEPARATE tool with the sensitive metric for the off-thread sweeper.
  * What actually perturbs the loop is **PawnIO driver contention** (the worker's
    own handle vs the control thread's energy read), not GPU/CPU load per se. The
    loop has documented, pre-existing, environmental multi-second
    `loop_work_duration_ms` spikes that cluster at GPU idle (old build: max
    3274 ms, 5 ticks > 1 s over 38 991, no feature at all). We bucket by GPU load
    only to compare like-with-like and stop that confound from dominating the
    verdict -- not because the sweeper's effect is load-dependent.

Discriminator: **p99-of-bulk** (per-tick cost after excluding > stall_ms
environmental spikes), compared within matched GPU buckets. `max` and the
overrun/spike counts are reported as CONTEXT only -- a single environmental stall
must never fail the gate.

The verdict is **PROVISIONAL**: no cycles-enabled capture exists yet, so the
tolerance is a labeled placeholder, not a validated threshold. Calibrate it by
running this on two existing cycles-OFF captures (baseline-vs-baseline) to measure
the natural run-to-run variance of `loop_work_duration_ms`; that off-vs-off
variance IS the real tolerance. Refine on the first enabled capture.

Usage:
  python score_loop_timing_gate.py --baseline <off.csv> --candidate <on.csv>
  python score_loop_timing_gate.py --baseline <offA.csv> --candidate <offB.csv>  # calibrate

Stdlib + the shared scripts/ helpers only.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from control_csv import column, parse_control_csv, to_float  # noqa: E402

# GPU-load bucket edges in milliwatts (gpu_power_mw is instantaneous board mW).
# Mirrors the FEAT-0020 validation buckets: idle < 150 W <= mid < 350 W <= load.
IDLE_MAX_MW = 150_000.0
LOAD_MIN_MW = 350_000.0
STALL_MS = 100.0           # ticks above this are environmental spikes (excluded from bulk)
OVERRUN_MS = 250.0         # the control period; work above it is an overrun
REL_TOL = 0.5              # PROVISIONAL p99-bulk tolerance (fraction) -- calibrate off-vs-off
ABS_TOL_MS = 2.0           # PROVISIONAL p99-bulk floor tolerance (ms)
BUCKETS = ("idle", "mid", "load", "unknown")


def _percentile(values, q):
    """Linear-interpolated percentile; None for an empty list."""
    xs = sorted(v for v in values if v is not None)
    if not xs:
        return None
    if len(xs) == 1:
        return xs[0]
    k = (len(xs) - 1) * (q / 100.0)
    f = math.floor(k)
    c = min(f + 1, len(xs) - 1)
    return xs[f] + (xs[c] - xs[f]) * (k - f)


def gpu_bucket(gpu_mw, idle_max_mw=IDLE_MAX_MW, load_min_mw=LOAD_MIN_MW):
    """GPU-load bucket from instantaneous board milliwatts."""
    if gpu_mw is None:
        return "unknown"
    if gpu_mw < idle_max_mw:
        return "idle"
    if gpu_mw < load_min_mw:
        return "mid"
    return "load"


def timing_stats(work_ms, stall_ms=STALL_MS, overrun_ms=OVERRUN_MS):
    """Distribution of one bucket's loop_work_duration_ms. p50/p95/p99/max over
    ALL ticks; p99_bulk excludes > stall_ms environmental spikes (the
    discriminator); spikes (> stall_ms) and overruns (> overrun_ms) are context.
    Pure; None values are dropped."""
    vals = [v for v in work_ms if v is not None]
    n = len(vals)
    if n == 0:
        return {"n": 0, "p50": None, "p95": None, "p99": None, "max": None,
                "p99_bulk": None, "spikes": 0, "overruns": 0}
    bulk = [v for v in vals if v <= stall_ms]
    return {
        "n": n,
        "p50": _percentile(vals, 50),
        "p95": _percentile(vals, 95),
        "p99": _percentile(vals, 99),
        "max": max(vals),
        "p99_bulk": _percentile(bulk, 99) if bulk else None,
        "spikes": sum(1 for v in vals if v > stall_ms),
        "overruns": sum(1 for v in vals if v > overrun_ms),
    }


def compare_bucket(base, cand, rel_tol=REL_TOL, abs_tol_ms=ABS_TOL_MS):
    """Provisional per-bucket verdict. ok=True/False on the p99-of-bulk
    discriminator (candidate <= max(base*(1+rel_tol), base+abs_tol_ms)); overruns
    and max are reported in the reason as context, never as a fail trigger so a
    single environmental spike cannot fail the gate. ok=None when either side
    lacks a bulk p99 (insufficient data in this bucket)."""
    if not base or not cand:
        return None, "no data"
    bp, cp = base.get("p99_bulk"), cand.get("p99_bulk")
    if bp is None or cp is None:
        return None, "insufficient bulk data in this bucket"
    allowed = max(bp * (1.0 + rel_tol), bp + abs_tol_ms)
    ok = cp <= allowed
    reason = (f"p99_bulk {bp:.2f} -> {cp:.2f} ms (allowed <= {allowed:.2f}); "
              f"overruns {base['overruns']} -> {cand['overruns']}, "
              f"max {base['max']:.1f} -> {cand['max']:.1f} ms [context]")
    return ok, reason


def bucketed_stats_from_csv(path, stall_ms=STALL_MS, overrun_ms=OVERRUN_MS,
                            idle_max_mw=IDLE_MAX_MW, load_min_mw=LOAD_MIN_MW):
    """Per-GPU-bucket timing_stats from a control-loop CSV, plus an 'all' rollup."""
    _meta, header, rows = parse_control_csv(path)
    work = [to_float(v) for v in column(header, rows, "loop_work_duration_ms")]
    gpu = [to_float(v) for v in column(header, rows, "gpu_power_mw")]
    by_bucket = {b: [] for b in BUCKETS}
    for w, gmw in zip(work, gpu):
        by_bucket[gpu_bucket(gmw, idle_max_mw, load_min_mw)].append(w)
    out = {b: timing_stats(v, stall_ms, overrun_ms) for b, v in by_bucket.items()}
    out["all"] = timing_stats(work, stall_ms, overrun_ms)
    return out


def _fmt(v):
    return "n/a" if v is None else f"{v:.2f}"


def render(base_stats, cand_stats, rel_tol, abs_tol_ms):
    """Human report + an overall provisional verdict. Returns (lines, verdict)."""
    lines = ["loop-timing gate (FEAT-0006 section-12, off-thread sweeper)",
             "  metric=loop_work_duration_ms  discriminator=p99_bulk  "
             f"(rel_tol={rel_tol}, abs_tol_ms={abs_tol_ms}) -- PROVISIONAL",
             "  bucket            base.p99_bulk  cand.p99_bulk   base.max  cand.max  ovr b->c   verdict"]
    comparable = 0
    failures = 0
    for b in ("idle", "mid", "load", "all"):
        base, cand = base_stats.get(b), cand_stats.get(b)
        ok, _reason = compare_bucket(base, cand, rel_tol, abs_tol_ms)
        if ok is None:
            verdict = "n/a"
        else:
            comparable += 1
            verdict = "ok" if ok else "MOVED"
            if not ok:
                failures += 1
        lines.append(
            f"  {b:<14}  {_fmt(base and base['p99_bulk']):>13}  "
            f"{_fmt(cand and cand['p99_bulk']):>13}  "
            f"{_fmt(base and base['max']):>8}  {_fmt(cand and cand['max']):>8}  "
            f"{(base or {}).get('overruns','?')!s:>3}->"
            f"{(cand or {}).get('overruns','?')!s:<3}  {verdict}")
    if comparable == 0:
        verdict = "INCONCLUSIVE"
    elif failures == 0:
        verdict = "PASS (provisional)"
    else:
        verdict = "MOVED (provisional)"
    lines.append(f"  => {verdict}  [{comparable} bucket(s) comparable, "
                 f"{failures} moved]")
    lines.append("  NOTE: provisional -- calibrate rel_tol/abs_tol_ms by running "
                 "baseline-vs-baseline on two cycles-OFF captures first; that "
                 "off-vs-off variance is the real tolerance.")
    return lines, verdict


def main(argv=None):
    ap = argparse.ArgumentParser(description="FEAT-0006 section-12 loop-timing gate")
    ap.add_argument("--baseline", required=True, type=Path,
                    help="cycles-OFF baseline control-loop CSV")
    ap.add_argument("--candidate", required=True, type=Path,
                    help="candidate control-loop CSV (cycles-ON, or a 2nd OFF for calibration)")
    ap.add_argument("--stall-ms", type=float, default=STALL_MS)
    ap.add_argument("--overrun-ms", type=float, default=OVERRUN_MS)
    ap.add_argument("--idle-max-w", type=float, default=IDLE_MAX_MW / 1000.0)
    ap.add_argument("--load-min-w", type=float, default=LOAD_MIN_MW / 1000.0)
    ap.add_argument("--rel-tol", type=float, default=REL_TOL)
    ap.add_argument("--abs-tol-ms", type=float, default=ABS_TOL_MS)
    ap.add_argument("--json", action="store_true", help="emit JSON instead of text")
    args = ap.parse_args(argv)

    idle_mw, load_mw = args.idle_max_w * 1000.0, args.load_min_w * 1000.0
    base = bucketed_stats_from_csv(args.baseline, args.stall_ms, args.overrun_ms,
                                   idle_mw, load_mw)
    cand = bucketed_stats_from_csv(args.candidate, args.stall_ms, args.overrun_ms,
                                   idle_mw, load_mw)
    lines, verdict = render(base, cand, args.rel_tol, args.abs_tol_ms)
    if args.json:
        print(json.dumps({"baseline": base, "candidate": cand,
                          "verdict": verdict}, indent=2))
    else:
        print("\n".join(lines))
    return 0 if verdict.startswith("PASS") else 1


if __name__ == "__main__":
    raise SystemExit(main())
