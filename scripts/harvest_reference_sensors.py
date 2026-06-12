#!/usr/bin/env python3
"""Harvest external CPU package-power reference sensors during a capture session.

Read-only. Samples, every --interval seconds into a timestamped CSV aligned to
the control-loop CSV's `wall_clock` (local %Y-%m-%dT%H:%M:%S):

  - hwinfo_cpu_pkg_w : HWiNFO shared memory, "Enhanced / CPU Package Power"
      (SMU-sourced => RAPL-INDEPENDENT; the criterion-3 reference).
  - lhm_cpu_pkg_w    : LibreHardwareMonitor http://localhost:8085/data.json,
      AMD Ryzen "Package" power (RAPL-derived => cross-check only, NOT
      independent of the energy MSR under test).

Provenance reasoning lives in
docs/cpu-energy-quarantine-exit-capture-runbook-2026-06-10.md; this tool only
collects. It writes one row per interval so the scorer can align the steady
load sub-window against the control-loop energy window.

On start it also writes `<out>.provenance.txt` listing EVERY HWiNFO
"CPU Package Power" [W] reading with its sensor group and marking the one
selected as the SMU reference. That makes the criterion-3 independence claim
auditable: a reviewer can confirm the selected sensor is the SMU/"Enhanced"
source and not a second, RAPL-derived "CPU Package Power" that would make the
+/-15% cross-check tautological.

Usage:
  python harvest_reference_sensors.py --seconds 1200 --interval 1.0 --out ref.csv
  python harvest_reference_sensors.py --until-stop --out ref.csv   # Ctrl-C to end
"""
from __future__ import annotations

import argparse
import json
import mmap
import struct
import sys
import time
import urllib.request
from datetime import datetime
from pathlib import Path

HWINFO_NAMES = ["Global\\HWiNFO_SENS_SM2", "Local\\HWiNFO_SENS_SM2",
                "HWiNFO_SENS_SM2"]
HWINFO_HEADER_FMT = "<IIIqIIIIII"
HWINFO_HEADER_SIZE = struct.calcsize(HWINFO_HEADER_FMT)
LHM_URL = "http://localhost:8085/data.json"


def _open_hwinfo(size):
    for name in HWINFO_NAMES:
        try:
            return mmap.mmap(-1, size, tagname=name, access=mmap.ACCESS_READ)
        except OSError:
            continue
    return None


def _cstr(buf, off, n):
    raw = buf[off:off + n]
    z = raw.find(b"\x00")
    if z >= 0:
        raw = raw[:z]
    return raw.decode("latin-1", "replace").strip()


def hwinfo_pkg_power_candidates():
    """Every HWiNFO 'CPU Package Power' [W] reading as (group, index, watts).

    Returns ALL matches regardless of sensor group so the SMU-vs-RAPL ambiguity
    (runbook 3) stays visible instead of being silently resolved by first-match.
    """
    m = _open_hwinfo(HWINFO_HEADER_SIZE)
    if m is None:
        return []
    try:
        (_sig, _ver, _rev, _pt, off_sens, sz_sens, n_sens,
         off_read, sz_read, n_read) = struct.unpack(
            HWINFO_HEADER_FMT, m[:HWINFO_HEADER_SIZE])
    finally:
        m.close()
    full = off_read + n_read * sz_read
    m = _open_hwinfo(full)
    if m is None:
        return []
    out = []
    try:
        sensor_names = [_cstr(m, off_sens + i * sz_sens + 8, 128)
                        for i in range(n_sens)]
        for i in range(n_read):
            base = off_read + i * sz_read
            label = _cstr(m, base + 12, 128)
            unit = _cstr(m, base + 268, 16)
            if unit != "W" or label != "CPU Package Power":
                continue
            sidx = struct.unpack_from("<I", m, base + 4)[0]
            sname = sensor_names[sidx] if sidx < len(sensor_names) else ""
            watts = struct.unpack_from("<d", m, base + 284)[0]
            out.append((sname, i, watts))
    finally:
        m.close()
    return out


def _is_smu_group(group):
    """The 'Enhanced' CPU sensor (group names the CPU/Ryzen) is the SMU source."""
    return "CPU" in group or "Ryzen" in group


def select_smu_candidate(candidates):
    """The (group, index, watts) used as the SMU reference, or (None, None, None)."""
    for group, idx, watts in candidates:
        if _is_smu_group(group):
            return group, idx, watts
    return None, None, None


def read_hwinfo_cpu_pkg_w():
    """The SMU 'CPU Package Power' (W) from HWiNFO shared memory, or None."""
    _group, _idx, watts = select_smu_candidate(hwinfo_pkg_power_candidates())
    return watts


def read_lhm_cpu_pkg_w(timeout=2.0):
    """The AMD Ryzen 'Package' power (W) from LHM, or None (RAPL-derived)."""
    try:
        with urllib.request.urlopen(LHM_URL, timeout=timeout) as resp:
            tree = json.loads(resp.read().decode("utf-8", "replace"))
    except Exception:
        return None

    found = [None]

    def walk(node, path):
        here = f"{path} / {node.get('Text', '')}" if path else node.get("Text", "")
        val = node.get("Value", "")
        if (node.get("Text") == "Package" and isinstance(val, str)
                and val.strip().endswith("W") and "Ryzen" in here):
            try:
                found[0] = float(val.strip().split()[0].replace(",", "."))
            except (ValueError, IndexError):
                pass
        for child in node.get("Children", []):
            walk(child, here)

    walk(tree, "")
    return found[0]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--seconds", type=float, default=0.0,
                        help="Duration; 0 with --until-stop runs until Ctrl-C.")
    parser.add_argument("--until-stop", action="store_true")
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)

    if args.seconds <= 0.0 and not args.until_stop:
        print("Specify --seconds N or --until-stop", file=sys.stderr)
        return 2

    # Criterion-3 independence audit: dump every 'CPU Package Power' [W] reading
    # and mark the one selected as the SMU reference, so a reviewer can confirm
    # it is the SMU/"Enhanced" source and not a RAPL-derived twin.
    candidates = hwinfo_pkg_power_candidates()
    sel_group, sel_idx, _sel_w = select_smu_candidate(candidates)
    prov_path = args.out.with_name(args.out.stem + ".provenance.txt")
    with prov_path.open("w", encoding="utf-8") as pf:
        pf.write("HWiNFO 'CPU Package Power' [W] readings at harvest start "
                 "(criterion-3 SMU-independence audit).\n"
                 "The SMU reference must NOT be a RAPL-derived sensor.\n\n")
        if not candidates:
            pf.write("  (none found -- HWiNFO not running / sensor disabled?)\n")
        for group, idx, watts in candidates:
            mark = " <== SELECTED as SMU reference" if idx == sel_idx else ""
            pf.write(f"  [{idx}] group='{group}'  {watts:.2f} W{mark}\n")
    if not args.quiet:
        print(f"pkg-power provenance -> {prov_path} "
              f"({len(candidates)} candidate(s), selected group='{sel_group}')")

    start = time.monotonic()
    deadline = start + args.seconds if args.seconds > 0 else None
    n = 0
    hw_ok = 0
    lhm_ok = 0
    with args.out.open("w", encoding="utf-8", newline="") as fh:
        fh.write("wall_clock,mono_s,hwinfo_cpu_pkg_w,lhm_cpu_pkg_w\n")
        try:
            while True:
                tick = time.monotonic()
                wall = datetime.now().strftime("%Y-%m-%dT%H:%M:%S")
                hw = read_hwinfo_cpu_pkg_w()
                lhm = read_lhm_cpu_pkg_w()
                n += 1
                hw_ok += hw is not None
                lhm_ok += lhm is not None
                fh.write(f"{wall},{tick - start:.3f},"
                         f"{'' if hw is None else f'{hw:.2f}'},"
                         f"{'' if lhm is None else f'{lhm:.2f}'}\n")
                fh.flush()
                if not args.quiet and n % 10 == 1:
                    print(f"[{wall}] hwinfo(SMU)={hw} lhm(RAPL)={lhm}")
                if deadline is not None and time.monotonic() >= deadline:
                    break
                sleep = args.interval - (time.monotonic() - tick)
                if sleep > 0:
                    time.sleep(sleep)
        except KeyboardInterrupt:
            pass

    print(f"harvested {n} samples -> {args.out} "
          f"(hwinfo {hw_ok}/{n}, lhm {lhm_ok}/{n})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
