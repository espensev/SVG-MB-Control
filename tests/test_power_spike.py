"""Unit tests for scripts/analyze_power_spike.py (transient-power Phase-B replay).

Pure-math helpers + a constructed end-to-end session where the package-power
slope rises before dTctl/dt; no exe, no hardware. The script module is loaded by
path because scripts/ is not a package.
"""
from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

from tests.helpers import REPO_ROOT

_SPEC = importlib.util.spec_from_file_location(
    "analyze_power_spike", REPO_ROOT / "scripts" / "analyze_power_spike.py")
aps = importlib.util.module_from_spec(_SPEC)
assert _SPEC.loader is not None
_SPEC.loader.exec_module(aps)


class PureMathTests(unittest.TestCase):
    def test_ema_constant_and_none(self) -> None:
        out = aps.ema([10.0, 10.0, 10.0], [1.0, 1.0, 1.0], 1.0)
        self.assertAlmostEqual(out[-1], 10.0, places=3)
        out2 = aps.ema([None, 10.0, None, 10.0], [1.0] * 4, 1.0)
        self.assertIsNone(out2[0])           # no sample yet
        self.assertAlmostEqual(out2[1], 10.0)
        self.assertAlmostEqual(out2[2], 10.0)  # None holds last value

    def test_slope_linear(self) -> None:
        s = aps.slope([0.0, 1.0, 2.0, 3.0], [1.0, 1.0, 1.0, 1.0])
        self.assertIsNone(s[0])
        self.assertAlmostEqual(s[1], 1.0)
        self.assertAlmostEqual(s[3], 1.0)

    def test_smootherstep_bounds(self) -> None:
        self.assertEqual(aps.smootherstep(-1.0), 0.0)
        self.assertEqual(aps.smootherstep(2.0), 1.0)
        self.assertAlmostEqual(aps.smootherstep(0.5), 0.5)

    def test_slope_drive_gate_and_band(self) -> None:
        kw = dict(s_start=20.0, s_full=120.0, w_floor=100.0, b_max=10.0)
        # below the level floor -> gated to 0
        self.assertEqual(aps.slope_drive(200.0, 50.0, **kw), 0.0)
        # below s_start -> 0
        self.assertEqual(aps.slope_drive(10.0, 150.0, **kw), 0.0)
        # at/above s_full and above floor -> full b_max
        self.assertAlmostEqual(aps.slope_drive(120.0, 150.0, **kw), 10.0)
        # mid-band -> strictly between
        mid = aps.slope_drive(70.0, 150.0, **kw)
        self.assertGreater(mid, 0.0)
        self.assertLess(mid, 10.0)
        # undefined input -> 0 (decay target)
        self.assertEqual(aps.slope_drive(None, 150.0, **kw), 0.0)

    def test_integrate_target_slews_and_clamps(self) -> None:
        # constant high target, fast rise -> reaches target; then target 0 -> falls.
        d = [10.0] * 10 + [0.0] * 10
        dts = [1.0] * 20
        b = aps.integrate_target(d, dts, rise=4.0, fall=2.0)
        self.assertLessEqual(b[0], 4.0 + 1e-9)        # first step clamped to +rise*dt
        self.assertAlmostEqual(b[9], 10.0, places=3)  # reached the target
        self.assertGreater(b[9] - b[10], 1.99)        # falls at -fall*dt
        self.assertLess(b[-1], b[9])

    def test_onset_50pct(self) -> None:
        times = [float(i) for i in range(11)]
        ramp = [0.0, 0.0, 0.0, 2.0, 4.0, 6.0, 8.0, 10.0, 10.0, 10.0, 10.0]
        # 50 % of 0..10 = 5 -> first index >=5 is i=5 (value 6) -> t=5.
        self.assertAlmostEqual(aps.onset_50pct(times, ramp, 0, 11), 5.0)

    def test_load_markers(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "m.csv"
            p.write_text("event,iso,unix_ms\nfloor,2026-06-10T10:00:00,1\n"
                         "burst_start,2026-06-10T10:02:00,2\n", encoding="utf-8")
            ms = aps.load_markers(p)
        self.assertEqual(len(ms), 2)
        self.assertEqual(ms[1]["event"], "burst_start")
        self.assertEqual(ms[1]["iso"], "2026-06-10T10:02:00")


def _make_spike_session(tmp: Path) -> Path:
    """60 rows @ 1 Hz. Floor watts 60 W until the burst at row 30, where the
    UN-MIRRORED watts steps to 180 W within a row; Tctl stays flat until row 32
    then ramps (so dTctl/dt rises AFTER the power slope). Busy pinned ~98 %."""
    rows = []
    for i in range(60):
        ss = i % 60
        wall = f"2026-06-10T10:{i // 60:02d}:{ss:02d}"
        busy = 98.0
        if i < 30:
            wu, tctl = 60.0, 60.0
        else:
            wu = 60.0 if i == 30 else 180.0
            if i < 32:
                tctl = 60.0
            elif i <= 40:
                tctl = 60.0 + (75.0 - 60.0) * ((i - 32) / 8.0)
            else:
                tctl = 75.0
        rows.append(f"{wall},{tctl},{busy},{wu}")

    header = "wall_clock,cpu_tctl_c,system_cpu_busy_pct,cpu_pkg_watts_unmirrored"
    (tmp / "session.csv").write_text(
        "# schema=svg_mb_control.log.v1\n# git_hash=deadbeef\n"
        "# config_sha256=0123456789abcdef\n"
        f"{header}\n" + "\n".join(rows) + "\n", encoding="utf-8")
    (tmp / "manifest.json").write_text(json.dumps({
        "load_mode": "spike",
        "phases": {"load_start": "2026-06-10T10:00:00",
                   "steady_start": "2026-06-10T10:00:05",
                   "steady_end": "2026-06-10T10:00:20"}}), encoding="utf-8")
    (tmp / "spike_markers.csv").write_text(
        "event,iso,unix_ms\nfloor,2026-06-10T10:00:00,0\n"
        "burst_start,2026-06-10T10:00:30,30000\n"
        "burst_end,2026-06-10T10:00:50,50000\n", encoding="utf-8")
    return tmp


class EndToEndTests(unittest.TestCase):
    def _run(self, d: Path) -> dict:
        return aps.analyze(d, tau_w=0.5, tau_s=0.5, deriv_half_s=1.0,
                           s_start=20.0, s_full=120.0, w_floor=100.0, b_max=10.0,
                           rise=40.0, fall=8.0, post_s=20.0)

    def test_power_slope_leads_dtctl(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            r = self._run(_make_spike_session(Path(td)))
        self.assertEqual(r["watts_source"], "unmirrored")
        self.assertEqual(r["bursts"], 1)
        b = r["per_burst"][0]
        self.assertTrue(b["aligned"])
        self.assertIsNotNone(b["power_slope_onset_s"])
        self.assertIsNotNone(b["dtctl_onset_s"])
        # power moved before the temperature derivative
        self.assertLess(b["power_slope_onset_s"], b["dtctl_onset_s"])
        self.assertGreater(r["differential_lead_median_s"], 0.0)

    def test_no_disturbance_floor_is_quiet(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            r = self._run(_make_spike_session(Path(td)))
        # floor watts (60 W) is below w_floor (100) and flat -> B^spk ~ 0.
        self.assertIsNotNone(r["bspk_floor_p95"])
        self.assertLess(r["bspk_floor_p95"], 0.5)
        self.assertGreaterEqual(r["busy_pinned"]["min"], 90.0)

    def test_markdown_renders(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            r = self._run(_make_spike_session(Path(td)))
            text = aps.render(r)
        self.assertIn("power-slope vs free dTctl/dt", text)
        self.assertIn("No-disturbance", text)


if __name__ == "__main__":
    unittest.main()
