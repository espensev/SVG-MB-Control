"""Unit tests for scripts/analyze_power_onset.py (Gate 2(a) event-onset math).

Pure-math + a constructed end-to-end session: no exe, no hardware,
platform-independent. The script module is loaded by path because scripts/ is
not a package.
"""
from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from datetime import datetime, timedelta
from pathlib import Path

from tests.helpers import REPO_ROOT

_SPEC = importlib.util.spec_from_file_location(
    "analyze_power_onset", REPO_ROOT / "scripts" / "analyze_power_onset.py")
apo = importlib.util.module_from_spec(_SPEC)
assert _SPEC.loader is not None
_SPEC.loader.exec_module(apo)

_BASE = datetime(2026, 6, 10, 10, 0, 0)
_FMT = "%Y-%m-%dT%H:%M:%S"


def _wall(i: int) -> str:
    return (_BASE + timedelta(seconds=i)).strftime(_FMT)


class HelperTests(unittest.TestCase):
    def test_secs_relative_and_bogus(self) -> None:
        anchor = datetime(2026, 6, 10, 10, 0, 30)
        self.assertEqual(apo._secs("2026-06-10T10:00:40", anchor), 10.0)
        self.assertEqual(apo._secs("2026-06-10T10:00:25", anchor), -5.0)
        self.assertIsNone(apo._secs("not-a-time", anchor))

    def test_quantile_small_and_empty(self) -> None:
        self.assertEqual(apo._quantile([10.0, 20.0, 30.0], 0.0), 10.0)
        self.assertIsNone(apo._quantile([], 0.10))
        # >=20 elements uses statistics.quantiles; p10 of 0..99 is ~9-10.
        big = [float(i) for i in range(100)]
        self.assertLessEqual(apo._quantile(big, 0.10), 12.0)

    def test_first_sustained_run_spike_and_none(self) -> None:
        times = [float(i) for i in range(12)]
        # short spike (2 rows) below sustain=3, then a sustained run at t=6.
        vals = [0, 0, 9, 9, 0, 0, 9, 9, 9, 9, 0, 0]
        self.assertEqual(apo._first_sustained(times, vals, 5.0, 3), 6.0)
        # a None interrupts the run.
        vals2 = [0, 9, None, 9, 9, 9, 0, 0, 0, 0, 0, 0]
        self.assertEqual(apo._first_sustained(times, vals2, 5.0, 3), 3.0)
        # never sustained -> None.
        self.assertIsNone(apo._first_sustained(times, [0] * 12, 5.0, 3))

    def test_levels_and_degenerate(self) -> None:
        times = [float(i) for i in range(-30, 60)]
        vals = [50.0 if t < 0 else 150.0 for t in times]
        lv = apo._levels(times, vals, pre_lo=-25, pre_hi=-10,
                         steady_a=20, steady_b=40, idle_quantile=0.10,
                         span_fraction=0.5)
        self.assertAlmostEqual(lv["idle"], 50.0)
        self.assertAlmostEqual(lv["steady"], 150.0)
        self.assertAlmostEqual(lv["threshold"], 100.0)
        # steady <= idle -> no threshold.
        flat = apo._levels(times, [50.0] * len(times), -25, -10, 20, 40, 0.10, 0.5)
        self.assertIsNone(flat["threshold"])


def _make_session(tmp: Path) -> Path:
    """A 75-row, 1 Hz session: 30 s idle then a load step at load_start.
    watts/busy step at t=0; Tctl ramps 55->80 C (2.5 C/s, capped)."""
    load_i = 30
    rows = []
    for i in range(75):
        t = i - load_i
        if t < 0:
            watts, busy, tctl = 50.0, 3.0, 55.0
        else:
            watts, busy = 150.0, 90.0
            tctl = min(80.0, 55.0 + t * 2.5)
        delta_uj = int(watts * 1000 * 1000)  # window 1000 ms -> W = uj/ms/1000
        rows.append(f"{_wall(i)},{tctl},{busy},{i + 1},1000,{delta_uj},quarantine")

    header = ("wall_clock,cpu_tctl_c,system_cpu_busy_pct,cpu_power_sample_id,"
              "cpu_power_window_ms,cpu_pkg_energy_delta_uj,"
              "cpu_pkg_energy_acquisition")
    (tmp / "session.csv").write_text(
        "# schema=svg_mb_control.log.v1\n# git_hash=deadbeef\n"
        "# config_sha256=0123456789abcdef\n"
        f"{header}\n" + "\n".join(rows) + "\n", encoding="utf-8")

    def w(off: int) -> str:
        return _wall(load_i + off)

    (tmp / "manifest.json").write_text(json.dumps({
        "session_label": "unit-test", "load_threads": 4,
        "phases": {
            "idle_start": w(-30), "load_start": w(0),
            "steady_start": w(20), "steady_end": w(40), "cooldown_start": w(43),
        },
    }), encoding="utf-8")
    return tmp


class EndToEndTests(unittest.TestCase):
    def _analyze(self, d: Path, trace_path: Path | None = None) -> dict:
        return apo.analyze(d, span_fraction=0.5, idle_quantile=0.10,
                           pre_load_window_s=25.0, onset_window_pre_s=10.0,
                           onset_window_post_s=60.0, sustain_s=2.0,
                           trace_path=trace_path)

    def test_onset_offsets_and_levels(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            r = self._analyze(_make_session(Path(td)))
        self.assertAlmostEqual(r["levels"]["watts"]["threshold"], 100.0)
        self.assertAlmostEqual(r["levels"]["tctl_c"]["threshold"], 67.5)
        # watts and busy step together at load_start; Tctl ramps to its 50 %
        # span point ~6 s later.
        self.assertAlmostEqual(r["onset_s"]["watts"], 0.0)
        self.assertAlmostEqual(r["onset_s"]["busy_pct"], 0.0)
        self.assertAlmostEqual(r["onset_s"]["tctl_c"], 6.0)
        self.assertAlmostEqual(r["deltas_s"]["watts_minus_busy"], 0.0)
        self.assertAlmostEqual(r["deltas_s"]["tctl_minus_watts"], 6.0)

    def test_tctl_named_thresholds(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            r = self._analyze(_make_session(Path(td)))
        self.assertAlmostEqual(r["tctl"]["max_c"], 80.0)
        self.assertAlmostEqual(r["tctl"]["cross_64c_s"], 4.0)
        self.assertIsNone(r["tctl"]["cross_84c_s"])  # capped at 80 C

    def test_trace_written_and_csv_row(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = _make_session(Path(td))
            trace = Path(td) / "trace.csv"
            r = self._analyze(d, trace_path=trace)
            lines = trace.read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines[0].split(","), apo.TRACE_FIELDS)
        self.assertEqual(len(lines), 1 + 75)  # header + one row per tick
        row = apo.csv_row(r)
        self.assertEqual(row["session_label"], "unit-test")
        self.assertEqual(row["onset_watts_s"], 0.0)
        self.assertEqual(row["watts_minus_busy_s"], 0.0)


if __name__ == "__main__":
    unittest.main()
