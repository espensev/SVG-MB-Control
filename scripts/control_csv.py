#!/usr/bin/env python3
"""Shared control-loop CSV helpers for the scripts/ analysis tools.

One source of truth for splitting a `schema=svg_mb_control.log.v1` CSV into
(meta, header, rows) and reading columns by name, used by
analyze_power_lead.py and score_energy_session.py. scripts/ is not a package;
consumers add this directory to sys.path before importing (the test loader
exercises them by file path).

Stdlib-only and read-only, like its consumers.
"""
from __future__ import annotations

import csv
import math
import re
from pathlib import Path

WALL_CLOCK_FMT = "%Y-%m-%dT%H:%M:%S"

# Per-CCD Tdie entries inside the amd_sensor_summary text column, e.g.
# "Tctl/Tdie=59.000 | CCD1 (Tdie)=39.500 | CCD2 (Tdie)=48.500". One source of
# truth so analyze_cpu_temp_power.py and cpu_config_fingerprint.py share it.
_CCD_TDIE_RE = re.compile(r"CCD(\d+) \(Tdie\)=([0-9.]+)")


def parse_ccd_temps(summary: str) -> dict[int, float]:
    """Map CCD index -> Tdie Celsius parsed from an amd_sensor_summary string.

    Returns an empty dict for an empty string or one with no CCD fields (e.g.
    a single-die part or a pre-decode row), never raising."""
    if not summary:
        return {}
    out: dict[int, float] = {}
    for match in _CCD_TDIE_RE.finditer(summary):
        try:
            out[int(match.group(1))] = float(match.group(2))
        except ValueError:
            continue
    return out


def parse_control_csv(path: Path) -> tuple[dict[str, str], list[str], list[list[str]]]:
    """Split a control-loop CSV into (#-comment meta, header, data rows)."""
    meta: dict[str, str] = {}
    header: list[str] = []
    rows: list[list[str]] = []
    with path.open(newline="", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\r\n")
            if not line:
                continue
            if line.startswith("#"):
                body = line.lstrip("#").strip()
                if "=" in body:
                    key, _, value = body.partition("=")
                    meta[key.strip()] = value.strip()
                continue
            if not header:
                header = next(csv.reader([line]))
                continue
            rows.append(next(csv.reader([line])))
    return meta, header, rows


def column(header: list[str], rows: list[list[str]], name: str) -> list[str]:
    """One column as raw strings ('' where the row is short)."""
    try:
        idx = header.index(name)
    except ValueError:
        return ["" for _ in rows]
    return [row[idx] if idx < len(row) else "" for row in rows]


def to_float(value: str) -> float | None:
    if value is None or value == "":
        return None
    try:
        out = float(value)
    except ValueError:
        return None
    return out if math.isfinite(out) else None
