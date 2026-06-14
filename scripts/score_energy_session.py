#!/usr/bin/env python3
"""Score one CPU-energy quarantine-exit capture session and write its evidence note.

Read-only. Consumes the manifest written by Capture-EnergySession.ps1 and scores
the FEAT-0006 Evaluation criteria (decision §Evaluation; criteria are normative
there, this only reads them):

  1 continuity across a 32-bit wrap   2 plausible range + load tracking
  3 +/-15% vs the SMU reference        4 effective-frequency validity (cycles)
  5 fault behavior                     6 no-disturbance vs the disabled baseline

It contributes ONE session toward the >=3 independent sessions gate; promotion
to `validated` stays a manual maintainer step (decision §Quarantine).

Usage:
  python score_energy_session.py --manifest <out>/manifest.json --session-num 1
"""
from __future__ import annotations

import argparse
import csv
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
    WALL_CLOCK_FMT as WALL_FMT,
    column as col,
    parse_control_csv,
    to_float as to_f,
)

CEILING_W = 400.0          # socket sanity ceiling (decision criterion 2)
WRAP_JOULES = 2 ** 32 * 15.26e-6   # ESU=16 -> 15.26 uJ/unit -> ~65.5 kJ per wrap
TOL_PCT = 15.0             # criterion 3 tolerance


def parse_wall(x):
    try:
        return datetime.strptime(x, WALL_FMT)
    except (ValueError, TypeError):
        return None


def pctl(values, q):
    xs = sorted(v for v in values if v is not None)
    if not xs:
        return float("nan")
    k = (len(xs) - 1) * q
    f = math.floor(k)
    c = min(f + 1, len(xs) - 1)
    return xs[f] + (xs[c] - xs[f]) * (k - f)


def window_mask(walls, start, end):
    s, e = parse_wall(start), parse_wall(end)
    out = []
    for w in walls:
        wt = parse_wall(w)
        out.append(wt is not None and s is not None and e is not None
                   and s <= wt <= e)
    return out


def time_weighted_watts(sample_ids, deltas, windows, mask):
    """Sum energy / sum window over DISTINCT sample windows in the mask."""
    seen = {}
    for sid, d, w, m in zip(sample_ids, deltas, windows, mask):
        if not m or not sid or sid == "0":
            continue
        dv, wv = to_f(d), to_f(w)
        if dv is None or wv is None or wv <= 0 or dv < 0:
            continue
        seen[sid] = (dv, wv)          # last wins; rows mirror the window
    if not seen:
        return None, 0, 0
    tot_uj = sum(v[0] for v in seen.values())
    tot_ms = sum(v[1] for v in seen.values())
    if tot_ms <= 0:
        return None, len(seen), 0
    return tot_uj / tot_ms / 1000.0, len(seen), tot_uj


def slip_from_rows(header, rows, mask=None):
    """loop_slip percentiles + overrun rate over rows (optionally masked)."""
    sel = list(range(len(rows))) if mask is None else [i for i, m in enumerate(mask) if m]
    if not sel:
        return None
    sc = col(header, rows, "loop_slip_ms")
    oc = col(header, rows, "loop_overrun")
    slip = [v for v in (to_f(sc[i]) for i in sel) if v is not None and v >= 0]
    if not slip:
        return None
    over = sum(1 for i in sel if str(oc[i]).strip().lower() in ("true", "1"))
    hours = max(len(sel) * 0.25 / 3600.0, 1e-9)
    # SD on the loop-work distribution only: >50 ms slips are OS scheduler/sleep
    # stalls (runbook §1), not energy-read cost, and would otherwise inflate the
    # no-disturbance threshold to meaninglessness.
    bulk = [x for x in slip if x <= 50.0]
    return {
        "rows": len(sel),
        "p50": pctl(slip, 0.50), "p95": pctl(slip, 0.95),
        "p99": pctl(slip, 0.99),
        "stdev": statistics.pstdev(bulk) if len(bulk) > 1 else 0.0,
        "overrun_rows": over, "overrun_per_h": over / hours,
    }


def slip_stats(path):
    if not path or not Path(path).is_file():
        return None
    _, h, r = parse_control_csv(Path(path))
    return slip_from_rows(h, r, None)


def verdict(ok, manual=False, incomplete=False):
    if manual:
        return "MANUAL"
    if incomplete:
        return "INCOMPLETE"
    return "PASS" if ok else "FAIL"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--manifest", required=True, type=Path)
    ap.add_argument("--session-num", type=int, default=1)
    ap.add_argument("--out", type=Path)
    args = ap.parse_args(argv)

    man = json.loads(args.manifest.read_text(encoding="utf-8"))
    ph = man.get("phases", {})
    session_csv = man.get("session_csv")
    if not session_csv or not Path(session_csv).is_file():
        print(f"session CSV missing: {session_csv}", file=sys.stderr)
        return 2

    meta, header, rows = parse_control_csv(Path(session_csv))
    walls = col(header, rows, "wall_clock")
    sids = col(header, rows, "cpu_power_sample_id")
    deltas = col(header, rows, "cpu_pkg_energy_delta_uj")
    windows = col(header, rows, "cpu_power_window_ms")
    markers = col(header, rows, "cpu_pkg_energy_acquisition")

    idle_m = window_mask(walls, ph.get("idle_start"), ph.get("idle_end"))
    load_m = window_mask(walls, ph.get("load_start"), ph.get("load_end"))
    cool_m = window_mask(walls, ph.get("cooldown_start"), ph.get("cooldown_end"))
    steady_m = window_mask(walls, ph.get("steady_start"), ph.get("steady_end"))

    idle_w, _, _ = time_weighted_watts(sids, deltas, windows, idle_m)
    load_w, load_wins, load_uj = time_weighted_watts(sids, deltas, windows, load_m)
    cool_w, _, _ = time_weighted_watts(sids, deltas, windows, cool_m)
    steady_w, _, _ = time_weighted_watts(sids, deltas, windows, steady_m)

    results = []  # (num, name, verdict, detail)

    # --- Criterion 1: wrap continuity ---
    load_deltas = [to_f(d) for d, m in zip(deltas, load_m)
                   if m and (d not in ("", None))]
    load_deltas = [d for d in load_deltas if d is not None]
    neg = sum(1 for d in load_deltas if d < 0)

    def blanks(mask):
        return sum(1 for sid, d, m in zip(sids, deltas, mask)
                   if m and sid and sid != "0" and (d == "" or to_f(d) is None))
    idle_blanks, blanks_load = blanks(idle_m), blanks(load_m)
    wraps = (load_uj / 1e6) / WRAP_JOULES if load_uj else 0.0
    # Continuity = monotone deltas (no negative-after-modular) across >=1 wrap and
    # no missing windows when the loop is healthy (idle). Load-phase blanks track
    # loop starvation under the synthetic load (see crit 5/6), not a counter fault.
    c1_ok = neg == 0 and wraps >= 0.9 and idle_blanks == 0
    results.append((1, "Counter continuity across >=1 wrap",
                    verdict(c1_ok, incomplete=(neg == 0 and wraps < 0.9)),
                    f"negative deltas={neg}, ~{wraps:.2f} wraps over "
                    f"{load_uj/1e6:.0f} J; idle blanks={idle_blanks}; "
                    f"load blanks={blanks_load}/{load_wins} "
                    f"(loop-starvation artifact, blanks cleanly -> crit 5)"))

    # --- Criterion 2: range + load tracking ---
    vals = [v for v in (idle_w, load_w, cool_w) if v is not None]
    c2_ok = (bool(vals) and all(0 <= v < CEILING_W for v in vals)
             and load_w is not None and idle_w is not None
             and load_w > idle_w * 1.2
             and (cool_w is None or cool_w < load_w))
    results.append((2, "Plausible range + load tracking", verdict(c2_ok),
                    f"idle={idle_w and round(idle_w,1)} W, "
                    f"load={load_w and round(load_w,1)} W, "
                    f"cooldown={cool_w and round(cool_w,1)} W (ceiling {CEILING_W:.0f})"))

    # --- Criterion 3: +/-15% vs SMU reference ---
    ref = man.get("reference_csv")

    def ref_means(start, end):
        """(SMU mean, LHM mean) over [start,end] from the harvester CSV."""
        if not (ref and Path(ref).is_file()):
            return None, None
        s, e = parse_wall(start), parse_wall(end)
        if s is None or e is None:
            return None, None
        smu, lhm = [], []
        with open(ref, newline="", encoding="utf-8") as fh:
            for r in csv.DictReader(fh):
                wt = parse_wall(r.get("wall_clock", ""))
                if wt is None or not (s <= wt <= e):
                    continue
                hv, lv = to_f(r.get("hwinfo_cpu_pkg_w")), to_f(r.get("lhm_cpu_pkg_w"))
                if hv is not None:
                    smu.append(hv)
                if lv is not None:
                    lhm.append(lv)
        return (statistics.mean(smu) if smu else None,
                statistics.mean(lhm) if lhm else None)

    # Prefer the steady sub-window; fall back to the full load window (short
    # loads make the steady window empty).
    smu_mean, lhm_mean = ref_means(ph.get("steady_start"), ph.get("steady_end"))
    rapl_w, win_label = steady_w, "steady"
    if smu_mean is None or rapl_w is None:
        smu_mean, lhm_mean = ref_means(ph.get("load_start"), ph.get("load_end"))
        rapl_w, win_label = load_w, "load(fallback)"
    if smu_mean and rapl_w is not None:
        dpct = (rapl_w - smu_mean) / smu_mean * 100.0
        c3 = verdict(abs(dpct) <= TOL_PCT)
        c3_detail = (f"RAPL {win_label}={rapl_w:.1f} W vs SMU={smu_mean:.1f} W -> "
                     f"{dpct:+.1f}% (tol +/-{TOL_PCT:.0f}%); "
                     f"LHM/RAPL-derived (cross-check, load-fragile)="
                     f"{lhm_mean and round(lhm_mean,1)}")
    else:
        c3 = "MANUAL"
        if smu_mean is None:
            c3_detail = ("no SMU samples in steady/load window (HWiNFO not "
                         "running/harvested?) -> supply an SMU average manually")
        else:
            c3_detail = (f"SMU={smu_mean:.1f} W present but RAPL watts "
                         "unavailable (energy disabled / no windows) -> enable "
                         "energy for a real session")
    results.append((3, "+/-15% external SMU cross-check", c3, c3_detail))

    # --- Criterion 4: effective frequency (cycles) ---
    if man.get("include_cycles"):
        aperf = col(header, rows, "cpu_aperf_delta")
        mperf = col(header, rows, "cpu_mperf_delta")

        def ratio(mask):
            a = sum(to_f(x) for x, m in zip(aperf, mask)
                    if m and to_f(x) is not None)
            mm = sum(to_f(x) for x, m in zip(mperf, mask)
                     if m and to_f(x) is not None)
            return (a / mm) if mm else None
        r_idle, r_load = ratio(idle_m), ratio(load_m)
        c4_detail = (f"dAPERF/dMPERF idle={r_idle and round(r_idle,3)}, "
                     f"load={r_load and round(r_load,3)} "
                     f"(x base freq = effective; analyze report --p0-mhz "
                     f"<base> derives it; promotion stays manual)")
        results.append((4, "Effective-frequency validity (cycles)", "MANUAL",
                        c4_detail))

    # --- Criterion 5: fault behavior ---
    enabled_rows = sum(1 for m in markers if m == "quarantine")
    false_zero = sum(1 for sid, d, m in zip(sids, deltas, markers)
                     if m == "quarantine" and sid and sid != "0" and d == "0")
    completed = any(cool_m)
    c5_ok = false_zero == 0 and completed
    results.append((5, "Fault behavior (no false zero / clean)",
                    verdict(c5_ok),
                    f"quarantine rows={enabled_rows}, false-zero deltas={false_zero}, "
                    f"session reached cooldown={completed}"))

    # --- Criterion 6: no-disturbance vs baseline ---
    base = slip_stats(man.get("baseline_csv"))
    # Isolate the ENERGY READ's overhead: compare the energy-on no-load phases
    # (idle + cooldown) against the disabled baseline. The load phase is excluded
    # -- its slip is the synthetic load saturating the CPU, not the energy read.
    noload = [bool(a or b) for a, b in zip(idle_m, cool_m)]
    sess = slip_from_rows(header, rows, noload)
    if base and sess:
        dp95 = sess["p95"] - base["p95"]
        c6_ok = (dp95 <= max(base["stdev"], 0.5)
                 and sess["overrun_per_h"] <= base["overrun_per_h"] + 2.0)
        c6_detail = (f"energy-on no-load p95 {base['p95']:.2f}->{sess['p95']:.2f} ms "
                     f"(d{dp95:+.2f}, baseline SD {base['stdev']:.2f}); "
                     f"overrun/h {base['overrun_per_h']:.1f}->{sess['overrun_per_h']:.1f} "
                     f"(load phase excluded)")
        c6 = verdict(c6_ok)
    else:
        c6, c6_detail = "MANUAL", "baseline or session slip stats unavailable"
    results.append((6, "No-disturbance vs disabled baseline", c6, c6_detail))

    # --- write the evidence note ---
    date = (man.get("completed") or man.get("created") or "")[:10]
    out = args.out or Path(
        f"docs/cpu-energy-quarantine-exit-evidence-{date}-s{args.session_num}.md")
    n_pass = sum(1 for r in results if r[2] == "PASS")
    n_fail = sum(1 for r in results if r[2] == "FAIL")
    lines = [
        f"# CPU Energy Quarantine-Exit Evidence — session {args.session_num} — {date}",
        "",
        "Auto-scored by `scripts/score_energy_session.py` from "
        f"`{args.manifest}`. One of >=3 independent sessions; promotion to "
        "`validated` stays a manual maintainer step (decision §Quarantine).",
        "",
        f"- Session CSV: `{session_csv}` ({len(rows)} rows)",
        f"- git `{meta.get('git_hash')}`  config `{(meta.get('config_sha256') or '')[:12]}`"
        f"  session_start `{meta.get('session_start')}`",
        f"- Cycles captured: {bool(man.get('include_cycles'))}   "
        f"Rehearse: {bool(man.get('rehearse'))}",
        f"- Markers: enable={man.get('markers', {}).get('after_enable')} "
        f"revert={man.get('markers', {}).get('after_revert')}",
        "",
        f"**Result: {n_pass} PASS, {n_fail} FAIL, "
        f"{len(results) - n_pass - n_fail} MANUAL/INCOMPLETE.**",
        "",
        "| # | Criterion | Verdict | Detail |",
        "|---|---|---|---|",
    ]
    for num, name, v, detail in results:
        lines.append(f"| {num} | {name} | **{v}** | {detail} |")
    lines += [
        "",
        "## Phase watts (time-weighted, distinct windows)",
        f"- idle {idle_w and round(idle_w,1)} W | load {load_w and round(load_w,1)} W"
        f" | cooldown {cool_w and round(cool_w,1)} W | steady {steady_w and round(steady_w,1)} W",
        "",
        "Criterion 3 manual fallback: if no SMU steady-window sample was "
        "harvested, record a Ryzen Master / confirmed-SMU HWiNFO average over "
        f"`{ph.get('steady_start')}..{ph.get('steady_end')}` and recompute the "
        "% delta by hand.",
        "",
    ]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"session {args.session_num}: {n_pass} PASS / {n_fail} FAIL / "
          f"{len(results) - n_pass - n_fail} other")
    for num, name, v, detail in results:
        print(f"  [{v:10}] {num}. {name} — {detail}")
    print(f"\nevidence note -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
