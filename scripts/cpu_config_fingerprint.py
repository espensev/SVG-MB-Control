#!/usr/bin/env python3
"""Auto-detect CPU config regimes (BIOS / Curve-Optimizer / PBO / voltage) from
telemetry alone, with NO operator annotation -- the operator will not log each
setting change, and assumed settings must never be hard-coded (see
docs/idle-cpu-temp-trend-and-enrichment-2026-06-20.md, "Principle: never assume
operator settings").

SKELETON. The dimensions that need the post-(-25 CO) cycles capture or the Vcore
probe (docs/cpu-cycles-capture-and-vcore-probe-plan-2026-06-21.md) degrade to None
until that data exists; everything else runs on the current logs today.

--- Merge contract: mirrors the new NVG + SVG logging conventions ---
This is deliberately built to converge with NVG-SmoothControl when the two
projects merge (verified against NVG docs/ANALYZER_RUNTIME_OBSERVABILITY_SPECS.md
and src/csv_logger.h, and SVG control_csv.py / analyze_*.py):

  1. Analyzer / reporting class ONLY -- never a control input. This is the change
     class both projects explicitly PREFER over controller-behavior changes.
  2. Additive + versioned output schema name `svg_mb_control.cpu_fingerprint.v1`
     (sibling of NVG `nvg_smooth.*.v1` and SVG `svg_mb_control.log.v1`). Add keys;
     never rename. Consumers ignore unknown keys.
  3. A per-regime rollup analogous to NVG `session_summary`; a "regime" is an
     auto-labeled segment, the analogue of NVG `load_marks` / `# campaign=` labels
     -- but discovered from data, not hand-typed.
  4. Convergent snake_case units: _c (Celsius) / _w / _mw / _mhz / _mv / _pct /
     _ms. New CPU columns mirror NVG naming (e.g. NVG logs `voltage_core_mv`, so
     promoted CPU voltage uses `cpu_voltage_core_mv`; dies use `cpu_ccd1_tdie_c`).
  5. Provenance carried like both loggers: git_hash, config_sha256, session_start.

Design rule (load-bearing): CONFIG-PURE dimensions drive segmentation; the
COOLING-OUTPUT scalar (theta C/W, Tctl-at-watt) is an OUTPUT only, so a repaste /
pump / ambient shift can never mint a false config regime.

Stdlib + the shared scripts/ helpers only (no numpy/pandas dependency).
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_power_lead import derive_watts  # noqa: E402
from control_csv import (  # noqa: E402
    column, parse_ccd_temps, parse_control_csv, to_float)

SCHEMA_NAME = "svg_mb_control.cpu_fingerprint.v1"

# Busy bands (system_cpu_busy_pct) at which package watts are compared across
# runs. A CO/PBO undervolt lowers watts at the same busy band.
BUSY_BANDS = (("idle", 0.0, 5.0), ("light", 30.0, 50.0), ("load", 95.0, 100.0))
IDLE_BUSY_HI = 5.0          # idle gate for the idle power/Vcore floor
MIN_SETTLED = 8             # per-run min samples before a dimension is trusted
# CUSUM-ish step gate: a config boundary needs a step > K*MAD that PERSISTS for
# at least M subsequent runs (a BIOS change is set once, not per-tick noise).
STEP_K_MAD = 4.0
STEP_PERSIST_RUNS = 2


# --------------------------------------------------------------------------- #
# small stdlib stats (kept local; mirrors analyze_cpu_temp_power._pct/_stats)   #
# --------------------------------------------------------------------------- #
def _finite(xs):
    return [x for x in xs if x is not None]


def _pct(xs, q):
    vals = sorted(_finite(xs))
    if not vals:
        return None
    i = round(q * (len(vals) - 1))
    return vals[max(0, min(len(vals) - 1, i))]


def _median(xs):
    vals = _finite(xs)
    return statistics.median(vals) if vals else None


def _mean(xs):
    vals = _finite(xs)
    return statistics.fmean(vals) if vals else None


def _mad(xs):
    """Median absolute deviation (robust scale). None if too few points."""
    vals = _finite(xs)
    if len(vals) < 2:
        return None
    med = statistics.median(vals)
    return statistics.median([abs(v - med) for v in vals])


def _round(v, n=3):
    return round(v, n) if isinstance(v, float) else v


# --------------------------------------------------------------------------- #
# per-run fingerprint                                                           #
# --------------------------------------------------------------------------- #
@dataclass
class RunFingerprint:
    # provenance (mirrors both loggers' metadata block)
    source_csv: str
    session_start: str | None
    git_hash: str | None
    config_sha256: str | None
    rows: int
    # acquisition modes -> used to pre-register NON-config boundaries
    energy_acq: str | None
    cycles_acq: str | None
    # --- CONFIG-PURE dimensions (drive segmentation) ---
    idle_power_floor_w: float | None = None
    power_at_busy_w: dict = field(default_factory=dict)   # band -> mean watts
    eff_mhz_per_w_by_band: dict = field(default_factory=dict)  # band -> MHz/W
    ccd_balance_c: float | None = None                    # p50(ccd2 - ccd1)
    idle_voltage_core_mv: float | None = None             # TODO: needs probe
    # --- COOLING-OUTPUT scalars (NOT segmentation inputs) ---
    tctl_idle_floor_c: float | None = None
    theta_c_per_w: float | None = None                    # needs --ambient-c
    # coverage
    idle_samples: int = 0


def _parse_ccd_balance(summaries: list[str]) -> float | None:
    """Median (CCD2 - CCD1) Tdie over the run, via the shared control_csv parser."""
    deltas = []
    for s in summaries:
        temps = parse_ccd_temps(s)
        if 1 in temps and 2 in temps:
            deltas.append(temps[2] - temps[1])
    return _median(deltas) if deltas else None


def _eff_mhz_by_window(sample_ids, aperf, mperf, p0_mhz):
    """Cycle-weighted effective MHz over DISTINCT cycle windows (mirrors the v10
    analyzer block). Returns one effective-MHz value per distinct sample id, or
    [] when cycles were disabled (the common case until the post-CO capture)."""
    seen: dict[str, tuple[float, float]] = {}
    for sid, da, dm in zip(sample_ids, aperf, mperf):
        sid = (sid or "").strip()
        if not sid or sid == "0" or da is None or dm is None or dm <= 0:
            continue
        seen.setdefault(sid, (da, dm))  # first row of each window
    out = []
    for da, dm in seen.values():
        out.append((da / dm) * p0_mhz)
    return out


def compute_run_fingerprint(path: Path, *, p0_mhz: float,
                            ambient_c: float | None) -> RunFingerprint:
    meta, header, rows = parse_control_csv(path)
    busy = [to_float(v) for v in column(header, rows, "system_cpu_busy_pct")]
    tctl = [to_float(v) for v in column(header, rows, "cpu_tctl_c")]
    watts = derive_watts(
        column(header, rows, "cpu_power_sample_id"),
        column(header, rows, "cpu_pkg_energy_delta_uj"),
        column(header, rows, "cpu_power_window_ms"),
    )
    aperf = [to_float(v) for v in column(header, rows, "cpu_aperf_delta")]
    mperf = [to_float(v) for v in column(header, rows, "cpu_mperf_delta")]
    cyc_ids = column(header, rows, "cpu_cycles_sample_id")
    summaries = column(header, rows, "amd_sensor_summary")
    # TODO(probe): once the SVI probe promotes a column, read it here, e.g.
    #   vcore = [to_float(v) for v in column(header, rows, "cpu_voltage_core_mv")]
    vcore = [None for _ in rows]

    def _acq(name):
        vals = [v for v in column(header, rows, name) if v]
        # the informative mode is the non-disabled one if present
        for v in vals:
            if v not in ("", "disabled"):
                return v
        return vals[0] if vals else None

    idle_idx = [i for i, b in enumerate(busy) if b is not None and b < IDLE_BUSY_HI]
    idle_watts = [watts[i] for i in idle_idx]
    idle_tctl = [tctl[i] for i in idle_idx]
    idle_vcore = [vcore[i] for i in idle_idx]

    power_at_busy = {}
    for name, lo, hi in BUSY_BANDS:
        band_w = [watts[i] for i, b in enumerate(busy)
                  if b is not None and lo <= b < hi]
        m = _mean(band_w)
        if m is not None and len(_finite(band_w)) >= MIN_SETTLED:
            power_at_busy[name] = round(m, 2)

    # effective MHz per busy band / mean watts in that band -> MHz per watt.
    eff_mw = {}
    eff_all = _eff_mhz_by_window(cyc_ids, aperf, mperf, p0_mhz)
    if eff_all:  # cycles were enabled for this run
        # SKELETON: a single run-level MHz/W from run-mean eff-MHz over run-mean
        # watts. TODO: bin eff-MHz by (busy, watts) window like power_at_busy so
        # the ratio is matched-load, not run-aggregate (needs per-window join of
        # cpu_cycles_sample_id to busy/watts).
        run_eff = _mean(eff_all)
        run_w = _mean(_finite(watts))
        if run_eff is not None and run_w:
            eff_mw["run"] = round(run_eff / run_w, 3)

    theta = None
    tctl_floor = _pct(idle_tctl, 0.05)
    if ambient_c is not None and tctl_floor is not None:
        # output-only; needs a matched-watt pairing for a real theta. Skeleton
        # uses the idle floor against ambient as a placeholder cooling scalar.
        idle_w = _pct(idle_watts, 0.50)
        if idle_w and idle_w > 0:
            theta = round((tctl_floor - ambient_c) / idle_w, 3)

    return RunFingerprint(
        source_csv=str(path),
        session_start=meta.get("session_start"),
        git_hash=(meta.get("git_hash") or None),
        config_sha256=(meta.get("config_sha256") or None),
        rows=len(rows),
        energy_acq=_acq("cpu_pkg_energy_acquisition"),
        cycles_acq=_acq("cpu_cycles_acquisition"),
        idle_power_floor_w=(round(_pct(idle_watts, 0.05), 2)
                            if _pct(idle_watts, 0.05) is not None
                            and len(_finite(idle_watts)) >= MIN_SETTLED else None),
        power_at_busy_w=power_at_busy,
        eff_mhz_per_w_by_band=eff_mw,
        ccd_balance_c=(round(_parse_ccd_balance(summaries), 2)
                       if _parse_ccd_balance(summaries) is not None else None),
        idle_voltage_core_mv=(round(_pct(idle_vcore, 0.50), 1)
                              if _pct(idle_vcore, 0.50) is not None else None),
        tctl_idle_floor_c=(round(tctl_floor, 2) if tctl_floor is not None
                           else None),
        theta_c_per_w=theta,
        idle_samples=len(idle_idx),
    )


# --------------------------------------------------------------------------- #
# segmentation: exact metadata boundaries + step-detection on config-pure dims  #
# --------------------------------------------------------------------------- #
# The dims that legitimately step on a settings change. Order = preference for
# attributing which dim triggered a boundary.
CONFIG_PURE_DIMS = (
    "idle_power_floor_w",
    "ccd_balance_c",
    "idle_voltage_core_mv",
)


def _dim_series(runs, dim):
    return [getattr(r, dim) for r in runs]


def _exact_boundaries(runs) -> set[int]:
    """Indices where the EXACT logged identity changes (free, certain cuts):
    git_hash, config_sha256. Plus acquisition-mode transitions, which we register
    as KNOWN NON-config boundaries (they make power/cycles appear/disappear and
    must never be claimed as a discovered config regime)."""
    cuts, non_config = set(), set()
    for i in range(1, len(runs)):
        prev, cur = runs[i - 1], runs[i]
        if (cur.git_hash or "") != (prev.git_hash or ""):
            cuts.add(i)
        if (cur.config_sha256 or "") != (prev.config_sha256 or ""):
            cuts.add(i)
        if (cur.energy_acq != prev.energy_acq) or (cur.cycles_acq != prev.cycles_acq):
            non_config.add(i)
    return cuts, non_config


def _step_boundaries(runs, non_config: set[int]) -> dict[int, list[str]]:
    """Detect persistent steps in config-pure dims. A boundary at i requires the
    pre-window median and the post-window (>= STEP_PERSIST_RUNS) median to differ
    by > STEP_K_MAD * MAD(series). Acquisition-transition indices are excluded so
    the 06-18 energy-on flip never reads as a config change."""
    found: dict[int, list[str]] = {}
    for dim in CONFIG_PURE_DIMS:
        series = _dim_series(runs, dim)
        scale = _mad(series)
        if scale is None or scale == 0:
            continue
        for i in range(1, len(runs)):
            if i in non_config:
                continue
            pre = _finite(series[max(0, i - STEP_PERSIST_RUNS):i])
            post = _finite(series[i:i + STEP_PERSIST_RUNS])
            if len(pre) < 1 or len(post) < STEP_PERSIST_RUNS:
                continue
            if abs(statistics.median(post) - statistics.median(pre)) > STEP_K_MAD * scale:
                found.setdefault(i, []).append(dim)
    return found


def segment(runs) -> list[dict]:
    runs = sorted(runs, key=lambda r: (r.session_start or "", r.source_csv))
    if not runs:
        return []
    exact, non_config = _exact_boundaries(runs)
    steps = _step_boundaries(runs, non_config)
    boundaries = sorted(exact | set(steps.keys()))

    regimes, start = [], 0
    cut_points = boundaries + [len(runs)]
    for n, end in enumerate(cut_points, start=1):
        members = runs[start:end]
        if not members:
            continue
        reasons = []
        if start in steps:
            reasons.append("step:" + "+".join(steps[start]))
        if start in exact:
            reasons.append("identity:git/config")
        tag = ("config-attributable" if any("step" in x for x in reasons)
               or any("identity" in x for x in reasons) else "initial")
        regimes.append({
            "regime": f"regime_{n}",
            "first_session_start": members[0].session_start,
            "last_session_start": members[-1].session_start,
            "runs": len(members),
            "trigger": reasons or ["initial"],
            "tag": tag,
            # session_summary-style rollup (NVG analogue): config-pure medians
            "summary": {
                "idle_power_floor_w": _round(_median(
                    [m.idle_power_floor_w for m in members])),
                "ccd_balance_c": _round(_median(
                    [m.ccd_balance_c for m in members])),
                "idle_voltage_core_mv": _round(_median(
                    [m.idle_voltage_core_mv for m in members])),
                # output-only, reported but NOT a segmentation input:
                "tctl_idle_floor_c__output": _round(_median(
                    [m.tctl_idle_floor_c for m in members])),
                "theta_c_per_w__output": _round(_median(
                    [m.theta_c_per_w for m in members])),
            },
            "member_git_hashes": sorted({m.git_hash for m in members if m.git_hash}),
        })
        start = end
    return regimes


# --------------------------------------------------------------------------- #
# discovery + CLI                                                               #
# --------------------------------------------------------------------------- #
def discover_csvs(runtime_home: Path) -> list[Path]:
    archive = runtime_home / "logs" / "archive"
    out = sorted(archive.glob("*control-loop*.csv")) if archive.is_dir() else []
    live = runtime_home / "logs" / "svg_mb_control_output.csv"
    if live.is_file():
        out.append(live)
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--runtime-home", type=Path, default=Path("release/runtime"))
    ap.add_argument("--csv", type=Path, nargs="*",
                    help="Explicit CSV list (overrides --runtime-home discovery).")
    ap.add_argument("--p0-mhz", type=float, default=4300.0,
                    help="Base (P0) MHz for eff-frequency. 9950X3D = 4300.")
    ap.add_argument("--ambient-c", type=float,
                    help="Room ambient C (manual). Enables the output-only theta.")
    ap.add_argument("--out", type=Path)
    args = ap.parse_args(argv)

    csvs = args.csv if args.csv else discover_csvs(args.runtime_home)
    if not csvs:
        print("No control-loop CSVs found.", file=sys.stderr)
        return 2

    fingerprints = []
    for path in csvs:
        if not path.is_file():
            continue
        try:
            fingerprints.append(compute_run_fingerprint(
                path, p0_mhz=args.p0_mhz, ambient_c=args.ambient_c))
        except Exception as exc:  # a malformed archive must not abort the sweep
            print(f"skip {path}: {exc}", file=sys.stderr)

    regimes = segment(fingerprints)
    result = {
        "schema": SCHEMA_NAME,
        "p0_mhz": args.p0_mhz,
        "ambient_c": args.ambient_c,
        "runs_scanned": len(fingerprints),
        "regimes": regimes,
        "runs": [asdict(f) for f in sorted(
            fingerprints, key=lambda r: (r.session_start or "", r.source_csv))],
        # coverage caveats so a reader never mistakes absence for signal
        "notes": {
            "eff_mhz_per_w": ("populated only for runs captured with "
                              "CPU_CYCLES_MODE=enabled (post-(-25 CO) capture)"),
            "idle_voltage_core_mv": "always None until the SVI Vcore probe promotes a column",
            "theta_c_per_w": "output-only; needs --ambient-c and a matched-watt pairing (TODO)",
        },
    }
    text = json.dumps(result, indent=2)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text + "\n", encoding="utf-8")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
