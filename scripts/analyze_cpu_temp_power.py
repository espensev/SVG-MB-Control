#!/usr/bin/env python3
"""Power-normalized CPU temperature evaluation for control-loop CSV logs.

This is an offline/read-only companion to Compare-CpuTemps.ps1. The older
harness answers "what CPU/Tctl did we see at sustained busy percent?" This one
answers "what CPU/Tctl did we see for sustained package watts, with GPU heat and
radiator response carried as context?"
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import sys
from collections import Counter
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_power_lead import derive_watts  # noqa: E402
from control_csv import (  # noqa: E402
    column, parse_ccd_temps, parse_control_csv, to_float)

RADIATOR_CHANNELS = (1, 4, 5)
CONTEXT_CHANNELS = (0, 2, 3)
DEFAULT_MACHINE_POLICY = Path("config/machines/snd-desk.cooling.policy.json")
DEFAULT_BANDS = "idle:0:60,moderate:60:100,high:100:160,extreme:160:"
TIME_COLUMNS = ("wall_clock", "snapshot_time")


@dataclass(frozen=True)
class PowerBand:
    name: str
    lo_w: float | None
    hi_w: float | None

    def label(self) -> str:
        lo = "" if self.lo_w is None else f"{self.lo_w:g}"
        hi = "" if self.hi_w is None else f"{self.hi_w:g}"
        if self.lo_w is None:
            return f"<{hi} W"
        if self.hi_w is None:
            return f">={lo} W"
        return f"{lo}-{hi} W"

    def contains(self, watts: float) -> bool:
        if self.lo_w is not None and watts < self.lo_w:
            return False
        if self.hi_w is not None and watts >= self.hi_w:
            return False
        return True


@dataclass
class Window:
    sample_id: str
    first_wall_clock: str
    last_wall_clock: str
    first_s: float
    last_s: float
    rows: int
    acquisition: str
    cpu_package_w: float
    cpu_tctl_c_avg: float
    cpu_tctl_c_max: float
    cpu_max_c_avg: float | None
    ccd1_tdie_c_avg: float | None
    ccd2_tdie_c_avg: float | None
    ccd_delta_c_avg: float | None
    system_cpu_busy_pct_avg: float | None
    gpu_memjn_c_avg: float | None
    gpu_power_w_avg: float | None
    gpu_power_w_max: float | None
    radiator_setpoint_avg_pct: float | None
    radiator_rpm_avg: float | None
    context_setpoint_avg_pct: float | None
    temp_rise_c: float | None
    theta_c_per_w: float | None
    gpu_confounded: bool


def _parse_time(text: str) -> datetime | None:
    if not text:
        return None
    text = text.strip()
    for fmt in ("%Y-%m-%dT%H:%M:%S", "%Y-%m-%dT%H:%M:%S.%f",
                "%Y-%m-%d %H:%M:%S"):
        try:
            return datetime.strptime(text, fmt)
        except ValueError:
            pass
    return None


def _seconds(dt: datetime) -> float:
    return (dt - datetime(1970, 1, 1)).total_seconds()


def _finite(values: Iterable[float | None]) -> list[float]:
    return [v for v in values if v is not None and math.isfinite(v)]


def _mean(values: Iterable[float | None]) -> float | None:
    xs = _finite(values)
    return statistics.fmean(xs) if xs else None


def _max(values: Iterable[float | None]) -> float | None:
    xs = _finite(values)
    return max(xs) if xs else None


def _pct(values: Iterable[float | None], q: float) -> float | None:
    xs = sorted(_finite(values))
    if not xs:
        return None
    index = round(q * (len(xs) - 1))
    return xs[max(0, min(len(xs) - 1, index))]


def _stats(values: Iterable[float | None]) -> dict[str, float | int | None]:
    xs = _finite(values)
    if not xs:
        return {"count": 0, "min": None, "p50": None, "p90": None,
                "max": None, "avg": None}
    return {
        "count": len(xs),
        "min": round(min(xs), 3),
        "p50": round(_pct(xs, 0.50) or 0.0, 3),
        "p90": round(_pct(xs, 0.90) or 0.0, 3),
        "max": round(max(xs), 3),
        "avg": round(statistics.fmean(xs), 3),
    }


def _round(value: float | None, digits: int = 3) -> float | None:
    if value is None or not math.isfinite(value):
        return None
    return round(value, digits)


def parse_power_bands(text: str) -> list[PowerBand]:
    bands: list[PowerBand] = []
    for raw in text.split(","):
        part = raw.strip()
        if not part:
            continue
        pieces = part.split(":")
        if len(pieces) != 3:
            raise argparse.ArgumentTypeError(
                "power bands must use name:lo:hi entries")
        name, lo_text, hi_text = pieces
        lo = None if lo_text == "" else float(lo_text)
        hi = None if hi_text == "" else float(hi_text)
        if not name:
            raise argparse.ArgumentTypeError("power band name cannot be blank")
        if lo is not None and hi is not None and lo >= hi:
            raise argparse.ArgumentTypeError(
                f"invalid band {name}: lo must be below hi")
        bands.append(PowerBand(name=name, lo_w=lo, hi_w=hi))
    if not bands:
        raise argparse.ArgumentTypeError("at least one power band is required")
    return bands


def _band_for(watts: float, bands: list[PowerBand]) -> PowerBand | None:
    for band in bands:
        if band.contains(watts):
            return band
    return None


def _first_existing(paths: list[Path]) -> Path | None:
    existing = [p for p in paths if p.is_file()]
    if not existing:
        return None
    return max(existing, key=lambda p: p.stat().st_mtime)


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


def _pick_time_column(header: list[str]) -> str:
    for name in TIME_COLUMNS:
        if name in header:
            return name
    raise SystemExit(
        "CSV must contain either wall_clock or snapshot_time for window timing")


def _numeric_column(header: list[str], rows: list[list[str]], name: str) -> list[float | None]:
    return [to_float(v) for v in column(header, rows, name)]


def _channel_numeric_column(header: list[str], rows: list[list[str]], *,
                            channel: int, suffix: str,
                            prefix: str = "channel",
                            fallback_prefix: str | None = None) -> list[float | None]:
    primary = f"{prefix}{channel}_{suffix}"
    if primary in header:
        return _numeric_column(header, rows, primary)
    if fallback_prefix is not None:
        fallback = f"{fallback_prefix}{channel}_{suffix}"
        if fallback in header:
            return _numeric_column(header, rows, fallback)
    return [None for _ in rows]


def _avg_channel_group(header: list[str], rows: list[list[str]],
                       channels: tuple[int, ...], suffix: str, *,
                       prefix: str = "channel",
                       fallback_prefix: str | None = None) -> list[float | None]:
    columns = [
        _channel_numeric_column(
            header, rows, channel=ch, suffix=suffix, prefix=prefix,
            fallback_prefix=fallback_prefix)
        for ch in channels
    ]
    out: list[float | None] = []
    for values in zip(*columns):
        out.append(_mean(values))
    return out


def _marker(markers: list[str]) -> str:
    cleaned = [m for m in markers if m]
    if not cleaned:
        return "(blank)"
    return Counter(cleaned).most_common(1)[0][0]


def load_channel_policy(path: Path | None) -> dict:
    if path is None or not path.is_file():
        return {
            "source": str(path) if path is not None else None,
            "radiator_channels": list(RADIATOR_CHANNELS),
            "context_channels": list(CONTEXT_CHANNELS),
            "excluded_channels": [],
            "loaded": False,
        }
    payload = json.loads(path.read_text(encoding="utf-8"))
    fans = payload.get("fans", [])
    radiator = sorted(
        int(f["channel"]) for f in fans
        if "radiator" in str(f.get("role", "")).lower()
    )
    groups = payload.get("groups", {})
    controlled = [
        int(ch) for ch in groups.get("controlled_airflow", {}).get("channels", [])
    ]
    excluded = [
        int(ch) for ch in groups.get("excluded_from_case_pressure", {}).get(
            "channels", [])
    ]
    if not radiator:
        radiator = list(RADIATOR_CHANNELS)
    if controlled:
        context = [ch for ch in controlled if ch not in radiator]
    else:
        context = list(CONTEXT_CHANNELS)
    return {
        "source": str(path),
        "radiator_channels": radiator,
        "context_channels": context,
        "excluded_channels": excluded,
        "loaded": True,
    }


def load_windows(path: Path, *, ambient_c: float | None,
                 gpu_power_confound_w: float,
                 gpu_mem_confound_c: float,
                 radiator_channels: tuple[int, ...] = RADIATOR_CHANNELS,
                 context_channels: tuple[int, ...] = CONTEXT_CHANNELS) -> tuple[dict[str, str], list[Window], dict]:
    meta, header, rows = parse_control_csv(path)
    required = [
        "cpu_tctl_c",
        "cpu_power_sample_id",
        "cpu_power_window_ms",
        "cpu_pkg_energy_delta_uj",
    ]
    missing = [name for name in required if name not in header]
    if missing:
        return meta, [], {
            "warning": f"CSV missing power evaluation column(s): {', '.join(missing)}",
            "rows": len(rows),
        }

    time_col = _pick_time_column(header)
    times_raw = column(header, rows, time_col)
    times = [_parse_time(v) for v in times_raw]
    sample_ids = column(header, rows, "cpu_power_sample_id")
    acquisitions = column(header, rows, "cpu_pkg_energy_acquisition")
    watts = derive_watts(
        sample_ids,
        column(header, rows, "cpu_pkg_energy_delta_uj"),
        column(header, rows, "cpu_power_window_ms"),
    )
    tctl = _numeric_column(header, rows, "cpu_tctl_c")
    cpu_max = _numeric_column(header, rows, "cpu_max_c")
    busy = _numeric_column(header, rows, "system_cpu_busy_pct")
    gpu_mem = _numeric_column(header, rows, "gpu_memjn_c")
    # Per-CCD Tdie from the amd_sensor_summary text (no schema change; the values
    # are already logged every tick). CCD1 = V-cache die, CCD2 = frequency die on
    # the 9950X3D; absent on single-die parts / pre-decode rows -> None.
    _ccd = [parse_ccd_temps(s) for s in column(header, rows, "amd_sensor_summary")]
    ccd1 = [t.get(1) for t in _ccd]
    ccd2 = [t.get(2) for t in _ccd]
    gpu_power_w = [
        None if v is None else v / 1000.0
        for v in _numeric_column(header, rows, "gpu_power_mw")
    ]
    rad_sp = _avg_channel_group(header, rows, radiator_channels, "setpoint_pct")
    rad_rpm = _avg_channel_group(
        header, rows, radiator_channels, "rpm", prefix="fan",
        fallback_prefix="channel")
    ctx_sp = _avg_channel_group(header, rows, context_channels, "setpoint_pct")

    groups: dict[str, list[int]] = {}
    dropped = {"missing_sample": 0, "blank_watts": 0, "bad_time": 0,
               "blank_tctl": 0}
    for i, sid in enumerate(sample_ids):
        sid = sid.strip()
        if not sid or sid == "0":
            dropped["missing_sample"] += 1
            continue
        if watts[i] is None:
            dropped["blank_watts"] += 1
            continue
        if times[i] is None:
            dropped["bad_time"] += 1
            continue
        if tctl[i] is None:
            dropped["blank_tctl"] += 1
            continue
        groups.setdefault(sid, []).append(i)

    windows: list[Window] = []
    for sid, idxs in groups.items():
        first = idxs[0]
        last = idxs[-1]
        cpu_w = _mean(watts[i] for i in idxs)
        cpu_t_avg = _mean(tctl[i] for i in idxs)
        cpu_t_max = _max(tctl[i] for i in idxs)
        if cpu_w is None or cpu_t_avg is None or cpu_t_max is None:
            continue
        gpu_w_avg = _mean(gpu_power_w[i] for i in idxs)
        gpu_w_max = _max(gpu_power_w[i] for i in idxs)
        gpu_mem_avg = _mean(gpu_mem[i] for i in idxs)
        ccd1_avg = _mean(ccd1[i] for i in idxs)
        ccd2_avg = _mean(ccd2[i] for i in idxs)
        ccd_deltas = [ccd2[i] - ccd1[i] for i in idxs
                      if ccd1[i] is not None and ccd2[i] is not None]
        ccd_delta_avg = _mean(ccd_deltas) if ccd_deltas else None
        temp_rise = None if ambient_c is None else cpu_t_avg - ambient_c
        theta = None
        if temp_rise is not None and cpu_w > 0.0:
            theta = temp_rise / cpu_w
        first_time = times[first]
        last_time = times[last]
        assert first_time is not None
        assert last_time is not None
        gpu_confounded = (
            (gpu_w_avg is not None and gpu_w_avg >= gpu_power_confound_w)
            or (gpu_mem_avg is not None and gpu_mem_avg >= gpu_mem_confound_c)
        )
        windows.append(Window(
            sample_id=sid,
            first_wall_clock=times_raw[first],
            last_wall_clock=times_raw[last],
            first_s=_seconds(first_time),
            last_s=_seconds(last_time),
            rows=len(idxs),
            acquisition=_marker([acquisitions[i] for i in idxs]),
            cpu_package_w=cpu_w,
            cpu_tctl_c_avg=cpu_t_avg,
            cpu_tctl_c_max=cpu_t_max,
            cpu_max_c_avg=_mean(cpu_max[i] for i in idxs),
            ccd1_tdie_c_avg=ccd1_avg,
            ccd2_tdie_c_avg=ccd2_avg,
            ccd_delta_c_avg=ccd_delta_avg,
            system_cpu_busy_pct_avg=_mean(busy[i] for i in idxs),
            gpu_memjn_c_avg=gpu_mem_avg,
            gpu_power_w_avg=gpu_w_avg,
            gpu_power_w_max=gpu_w_max,
            radiator_setpoint_avg_pct=_mean(rad_sp[i] for i in idxs),
            radiator_rpm_avg=_mean(rad_rpm[i] for i in idxs),
            context_setpoint_avg_pct=_mean(ctx_sp[i] for i in idxs),
            temp_rise_c=temp_rise,
            theta_c_per_w=theta,
            gpu_confounded=gpu_confounded,
        ))

    windows.sort(key=lambda w: (w.first_s, w.sample_id))
    return meta, windows, {
        "rows": len(rows),
        "dropped": dropped,
        "time_column": time_col,
        "warning": None,
    }


def _settled_mask(windows: list[Window], bands: list[PowerBand],
                  dwell_s: float, max_gap_s: float) -> tuple[dict[str, str], dict[str, dict[str, int]]]:
    per_window_band: list[PowerBand | None] = [
        _band_for(w.cpu_package_w, bands) for w in windows
    ]
    state: dict[str, str] = {}
    counts = {
        band.name: {"in_band": 0, "settled": 0, "ramp": 0, "no_dwell": 0}
        for band in bands
    }
    for i, band in enumerate(per_window_band):
        if band is None:
            state[windows[i].sample_id] = "out_of_band"
            continue
        counts[band.name]["in_band"] += 1
        window_start = windows[i].first_s - dwell_s
        prev_start = windows[i].first_s
        reached = False
        reason = "settled"
        j = i - 1
        while j >= 0:
            if (prev_start - windows[j].last_s) > max_gap_s:
                reason = "no_dwell"
                break
            if windows[j].first_s <= window_start:
                reached = True
                break
            if per_window_band[j] is None or per_window_band[j].name != band.name:
                reason = "ramp"
                break
            prev_start = windows[j].first_s
            j -= 1
        if reason == "settled" and not reached:
            reason = "no_dwell"
        state[windows[i].sample_id] = reason
        counts[band.name]["settled" if reason == "settled" else reason] += 1
    return state, counts


def _summarize_windows(windows: list[Window]) -> dict:
    return {
        "windows": len(windows),
        "cpu_package_w": _stats(w.cpu_package_w for w in windows),
        "cpu_tctl_c_avg": _stats(w.cpu_tctl_c_avg for w in windows),
        "cpu_tctl_c_max": _stats(w.cpu_tctl_c_max for w in windows),
        "ccd1_tdie_c": _stats(w.ccd1_tdie_c_avg for w in windows),
        "ccd2_tdie_c": _stats(w.ccd2_tdie_c_avg for w in windows),
        "ccd_delta_c": _stats(w.ccd_delta_c_avg for w in windows),
        "temp_rise_c": _stats(w.temp_rise_c for w in windows),
        "theta_c_per_w": _stats(w.theta_c_per_w for w in windows),
        "system_cpu_busy_pct_avg": _stats(w.system_cpu_busy_pct_avg for w in windows),
        "gpu_power_w_avg": _stats(w.gpu_power_w_avg for w in windows),
        "gpu_memjn_c_avg": _stats(w.gpu_memjn_c_avg for w in windows),
        "radiator_setpoint_avg_pct": _stats(w.radiator_setpoint_avg_pct for w in windows),
        "radiator_rpm_avg": _stats(w.radiator_rpm_avg for w in windows),
    }


def analyze(path: Path, *, ambient_c: float | None, bands: list[PowerBand],
            dwell_s: float, max_gap_s: float,
            gpu_power_confound_w: float,
            gpu_mem_confound_c: float,
            channel_policy: dict | None = None) -> dict:
    policy = channel_policy or load_channel_policy(None)
    radiator_channels = tuple(int(ch) for ch in policy["radiator_channels"])
    context_channels = tuple(int(ch) for ch in policy["context_channels"])
    meta, windows, load_info = load_windows(
        path,
        ambient_c=ambient_c,
        gpu_power_confound_w=gpu_power_confound_w,
        gpu_mem_confound_c=gpu_mem_confound_c,
        radiator_channels=radiator_channels,
        context_channels=context_channels,
    )
    state, gate_counts = _settled_mask(windows, bands, dwell_s, max_gap_s)
    by_band: dict[str, dict] = {}
    for band in bands:
        in_band = [w for w in windows if _band_for(w.cpu_package_w, bands) == band]
        settled = [w for w in in_band if state.get(w.sample_id) == "settled"]
        unconfounded = [w for w in settled if not w.gpu_confounded]
        by_band[band.name] = {
            "range": band.label(),
            "gate": gate_counts[band.name],
            "all_settled": _summarize_windows(settled),
            "gpu_unconfounded_settled": _summarize_windows(unconfounded),
        }
    acquisition = Counter(w.acquisition for w in windows)
    return {
        "csv": str(path),
        "session": {k: meta.get(k) for k in
                    ("session_start", "git_hash", "config_sha256", "mode")},
        "rows": load_info["rows"],
        "time_column": load_info.get("time_column"),
        "ambient_c": ambient_c,
        "dwell_s": dwell_s,
        "max_gap_s": max_gap_s,
        "gpu_confound_thresholds": {
            "gpu_power_w": gpu_power_confound_w,
            "gpu_memjn_c": gpu_mem_confound_c,
        },
        "channel_policy": policy,
        "power_bands": [asdict(b) | {"range": b.label()} for b in bands],
        "windows": len(windows),
        "first_window": windows[0].first_wall_clock if windows else None,
        "last_window": windows[-1].last_wall_clock if windows else None,
        "acquisition_windows": dict(sorted(acquisition.items())),
        "dropped": load_info.get("dropped", {}),
        "warning": load_info.get("warning"),
        "overall": _summarize_windows(windows),
        "bands": by_band,
        "_windows": windows,
    }


def _fmt(value: float | int | None, digits: int = 2) -> str:
    if value is None:
        return "-"
    if isinstance(value, int):
        return str(value)
    return f"{value:.{digits}f}"


def _cell(stats: dict, key: str, digits: int = 2) -> str:
    s = stats.get(key) or {}
    if not s or not s.get("count"):
        return "-"
    return "/".join(_fmt(s[name], digits) for name in ("p50", "p90", "max"))


def render_markdown(result: dict) -> str:
    sess = result["session"]
    lines = [
        "# CPU temperature by package power",
        "",
        f"- CSV: `{result['csv']}`",
        f"- Session: start {sess.get('session_start')}, git `{sess.get('git_hash')}`, config `{(sess.get('config_sha256') or '')[:12]}`",
        f"- Windows: {result['windows']} distinct CPU package-energy windows from {result['first_window']} to {result['last_window']}",
        f"- Ambient: {_fmt(result['ambient_c'], 1)} C",
        f"- Dwell gate: {result['dwell_s']:.0f} s continuous same power band, max gap {result['max_gap_s']:.0f} s",
        f"- GPU confound: GPU board >= {result['gpu_confound_thresholds']['gpu_power_w']:.0f} W or GPU mem junction >= {result['gpu_confound_thresholds']['gpu_memjn_c']:.0f} C",
        f"- Radiator channels: {', '.join(str(ch) for ch in result['channel_policy']['radiator_channels'])}; context channels: {', '.join(str(ch) for ch in result['channel_policy']['context_channels'])}",
        "",
    ]
    if result.get("warning"):
        lines.extend([f"> {result['warning']}", ""])
    lines.extend([
        "## Power-band summary",
        "",
        "Values are p50/p90/max. `theta` is `(CPU Tctl avg - ambient C) / CPU package W`; use it as an apparent, run-local C/W score, not an absolute cooler spec.",
        "",
        "| Band | Range | settled/all | unconfounded | CPU W | Tctl avg C | Tctl max C | CCD2-CCD1 C | theta C/W | busy % | GPU W | rad setpoint % | rad RPM |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ])
    for name, band in result["bands"].items():
        gate = band["gate"]
        all_s = band["all_settled"]
        clean = band["gpu_unconfounded_settled"]
        lines.append(
            f"| {name} | {band['range']} | {gate['settled']}/{gate['in_band']} | "
            f"{clean['windows']} | {_cell(all_s, 'cpu_package_w')} | "
            f"{_cell(all_s, 'cpu_tctl_c_avg')} | {_cell(all_s, 'cpu_tctl_c_max')} | "
            f"{_cell(all_s, 'ccd_delta_c')} | "
            f"{_cell(all_s, 'theta_c_per_w', 3)} | "
            f"{_cell(all_s, 'system_cpu_busy_pct_avg', 1)} | "
            f"{_cell(all_s, 'gpu_power_w_avg', 1)} | "
            f"{_cell(all_s, 'radiator_setpoint_avg_pct', 1)} | "
            f"{_cell(all_s, 'radiator_rpm_avg', 0)} |"
        )
    lines.extend([
        "",
        "## Reading the result",
        "",
        "- Use the existing `Compare-CpuTemps.ps1` ledger for the multi-day temperature trend by CPU busy band.",
        "- Use this report when power fields are present: compare same power band, same ambient, similar GPU confound state, and similar radiator RPM/setpoint.",
        "- Treat GPU-confounded rows separately. A 500-600 W GPU load changes case and coolant conditions enough that CPU temperature is not CPU-only cooler evidence.",
        "- Prefer `theta C/W` and Tctl-at-power over raw Tctl when comparing cooling/settings, because the same temperature at 50 W and 100 W means different thermal performance.",
    ])
    return "\n".join(lines) + "\n"


WINDOW_FIELDS = [
    "sample_id", "first_wall_clock", "last_wall_clock", "rows", "acquisition",
    "cpu_package_w", "cpu_tctl_c_avg", "cpu_tctl_c_max", "cpu_max_c_avg",
    "ccd1_tdie_c_avg", "ccd2_tdie_c_avg", "ccd_delta_c_avg",
    "temp_rise_c", "theta_c_per_w", "system_cpu_busy_pct_avg",
    "gpu_memjn_c_avg", "gpu_power_w_avg", "gpu_power_w_max",
    "gpu_confounded", "radiator_setpoint_avg_pct", "radiator_rpm_avg",
    "context_setpoint_avg_pct",
]


def write_window_csv(path: Path, windows: list[Window]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=WINDOW_FIELDS)
        writer.writeheader()
        for window in windows:
            raw = asdict(window)
            row = {
                key: (_round(raw[key]) if isinstance(raw.get(key), float)
                      else raw.get(key))
                for key in WINDOW_FIELDS
            }
            writer.writerow(row)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--csv", type=Path,
                        help="Control-loop CSV path. Defaults to latest runtime log.")
    parser.add_argument("--runtime-home", type=Path,
                        default=Path("release/runtime"),
                        help="Runtime home used to find latest CSV when --csv is omitted.")
    parser.add_argument("--machine-policy", type=Path,
                        default=DEFAULT_MACHINE_POLICY,
                        help="Machine cooling policy JSON used to identify radiator/context channels.")
    parser.add_argument("--ambient-c", type=float,
                        help="Room ambient C. Enables temp-rise and theta C/W.")
    parser.add_argument("--power-bands", type=parse_power_bands,
                        default=parse_power_bands(DEFAULT_BANDS),
                        help=("Comma-separated name:lo:hi watt bands. Default "
                              f"{DEFAULT_BANDS!r}. Blank lo/hi means open-ended."))
    parser.add_argument("--dwell-s", type=float, default=60.0,
                        help="Continuous same-power-band seconds before a window counts.")
    parser.add_argument("--max-gap-s", type=float, default=5.0,
                        help="Max allowed timing gap inside the dwell window.")
    parser.add_argument("--gpu-power-confound-w", type=float, default=300.0,
                        help="GPU board watts that marks a CPU window confounded.")
    parser.add_argument("--gpu-mem-confound-c", type=float, default=60.0,
                        help="GPU memory junction C that marks a CPU window confounded.")
    parser.add_argument("--format", choices=("markdown", "json"),
                        default="markdown")
    parser.add_argument("--out", type=Path, help="Write report here.")
    parser.add_argument("--json-out", type=Path,
                        help="Write JSON summary here regardless of --format.")
    parser.add_argument("--window-csv-out", type=Path,
                        help="Write one row per CPU package-energy window.")
    args = parser.parse_args(argv)

    csv_path = args.csv if args.csv is not None else latest_control_csv(args.runtime_home)
    if not csv_path.is_file():
        print(f"CSV not found: {csv_path}", file=sys.stderr)
        return 2
    result = analyze(
        csv_path,
        ambient_c=args.ambient_c,
        bands=args.power_bands,
        dwell_s=args.dwell_s,
        max_gap_s=args.max_gap_s,
        gpu_power_confound_w=args.gpu_power_confound_w,
        gpu_mem_confound_c=args.gpu_mem_confound_c,
        channel_policy=load_channel_policy(args.machine_policy),
    )
    windows = result.pop("_windows")
    if args.window_csv_out:
        write_window_csv(args.window_csv_out, windows)
    json_text = json.dumps(result, indent=2)
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json_text + "\n", encoding="utf-8")
    text = json_text + "\n" if args.format == "json" else render_markdown(result)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding="utf-8")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
