"""Unit tests for scripts/analyze_power_lead.py (Gate 2 characterization math).

Pure-math tests: no exe, no hardware, platform-independent. The script module
is loaded by path because scripts/ is not a package.
"""
from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

from tests.helpers import REPO_ROOT

_SPEC = importlib.util.spec_from_file_location(
    "analyze_power_lead", REPO_ROOT / "scripts" / "analyze_power_lead.py")
apl = importlib.util.module_from_spec(_SPEC)
assert _SPEC.loader is not None
_SPEC.loader.exec_module(apl)


def make_csv(tmp: Path, rows: list[str]) -> Path:
    path = tmp / "session.csv"
    header = ("wall_clock,cpu_tctl_c,system_cpu_busy_pct,cpu_power_sample_id,"
              "cpu_power_window_ms,cpu_pkg_energy_delta_uj,"
              "cpu_pkg_energy_acquisition")
    path.write_text(
        "# schema=svg_mb_control.log.v1\n"
        "# git_hash=deadbeef\n"
        "# config_sha256=0123456789abcdef\n"
        f"{header}\n" + "\n".join(rows) + "\n",
        encoding="utf-8")
    return path


class ParseAndDeriveTests(unittest.TestCase):
    def test_parse_control_csv_splits_meta_header_rows(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = make_csv(Path(td), [
                "2026-06-10T10:00:00,50.0,3.0,1,1000,45000000,quarantine",
                "2026-06-10T10:00:01,50.5,3.0,1,1000,45000000,quarantine",
            ])
            meta, header, rows = apl.parse_control_csv(path)
        self.assertEqual(meta["git_hash"], "deadbeef")
        self.assertEqual(header[0], "wall_clock")
        self.assertEqual(len(rows), 2)

    def test_derive_watts(self) -> None:
        watts = apl.derive_watts(
            ["0", "1", "2", "3", "4"],
            ["1000", "200000000", "", "-5", "45000000"],
            ["1000", "1000", "1000", "1000", "0"])
        self.assertIsNone(watts[0])  # sample_id 0: no window yet
        self.assertAlmostEqual(watts[1], 200.0)
        self.assertIsNone(watts[2])  # blanked delta
        self.assertIsNone(watts[3])  # negative delta
        self.assertIsNone(watts[4])  # non-positive window

    def test_tick_seconds_from_span(self) -> None:
        # 41 rows over 10 seconds -> 0.25 s/row.
        wall = ["2026-06-10T10:00:00"] * 0 + [
            f"2026-06-10T10:00:{s:02d}" for s in range(11) for _ in range(4)
        ][:41]
        self.assertAlmostEqual(apl.tick_seconds(wall), 10.0 / 40.0)

    def test_tick_seconds_fallback(self) -> None:
        self.assertEqual(apl.tick_seconds(["bogus", "alsobogus"]),
                         apl.DEFAULT_TICK_S)


class CorrelationTests(unittest.TestCase):
    def test_pearson_perfect_and_degenerate(self) -> None:
        xs = [float(i) for i in range(32)]
        self.assertAlmostEqual(apl.pearson(xs, xs), 1.0)
        self.assertIsNone(apl.pearson([1.0] * 32, xs))  # zero variance
        self.assertIsNone(apl.pearson(xs[:4], xs[:4]))  # below min pairs

    def test_best_lead_recovers_constructed_lag(self) -> None:
        # Driver steps at index 40; response is the driver delayed 24 ticks.
        # At dt 0.25 s the constructed lead is 6.0 s.
        n, lag = 200, 24
        driver = [0.0] * 40 + [1.0] * (n - 40)
        response = [0.0] * lag + driver[: n - lag]
        out = apl.best_lead(driver, response, dt_s=0.25, max_lag_s=30.0)
        self.assertIsNotNone(out)
        self.assertAlmostEqual(out["lead_s"], 6.0)
        self.assertAlmostEqual(out["r"], 1.0)

    def test_smoothed_derivative_recovers_slope(self) -> None:
        # 2 C/s ramp sampled at 0.25 s.
        values = [0.5 * i for i in range(40)]
        deriv = apl.smoothed_derivative(values, dt_s=0.25, half_window_s=1.0)
        self.assertIsNone(deriv[0])
        self.assertAlmostEqual(deriv[20], 2.0)


class StatsAndCoverageTests(unittest.TestCase):
    def test_split_by_busy_and_stats(self) -> None:
        watts = [40.0, 41.0, 200.0, 210.0]
        busy = [1.0, 2.0, 80.0, None]
        idle, load = apl.split_by_busy(watts, busy, idle_below_pct=5.0)
        self.assertEqual(idle, [40.0, 41.0])
        self.assertEqual(load, [200.0])  # None busy row excluded
        stats = apl.series_stats(idle)
        self.assertEqual(stats["count"], 2)
        self.assertAlmostEqual(stats["p50"], 40.5)
        self.assertIsNone(apl.series_stats([None, None]))

    def test_window_coverage(self) -> None:
        cov = apl.window_coverage(
            ["0", "1", "1", "2", "2"],
            [None, 200.0, 200.0, None, None],
            ["disabled", "quarantine", "quarantine", "quarantine", "quarantine"])
        self.assertEqual(cov["distinct_windows"], 2)
        self.assertEqual(cov["blanked_windows"], 1)
        self.assertEqual(cov["acquisition_rows"]["quarantine"], 4)


class EndToEndTests(unittest.TestCase):
    def test_analyze_disabled_session_reports_busy_legs(self) -> None:
        # No energy windows at all: power legs n/a, busy legs still computed.
        rows = []
        for i in range(120):
            busy = 2.0 if i < 40 else 90.0
            tctl = 50.0 if i < 48 else 70.0  # responds 8 rows after busy
            rows.append(
                f"2026-06-10T10:{i // 60:02d}:{i % 60:02d},{tctl},{busy},,,,disabled")
        with tempfile.TemporaryDirectory() as td:
            path = make_csv(Path(td), rows)
            result = apl.analyze(path, max_lag_s=30.0, idle_busy_pct=5.0,
                                 deriv_half_window_s=1.0)
            text = apl.render_markdown(result)
        self.assertEqual(result["coverage"]["distinct_windows"], 0)
        self.assertIsNone(result["lead"]["watts_to_tctl"])
        busy_lead = result["lead"]["busy_to_tctl"]
        self.assertIsNotNone(busy_lead)
        self.assertGreater(busy_lead["lead_s"], 0.0)
        self.assertIn("No energy windows", text)

    def test_analyze_enabled_session_power_lead(self) -> None:
        # Watts step leads the Tctl step by 12 rows (12 s at 1 row/s).
        rows = []
        for i in range(120):
            sid = i + 1
            delta = 45000000 if i < 40 else 200000000
            tctl = 50.0 if i < 52 else 75.0
            busy = 2.0 if i < 40 else 95.0
            rows.append(
                f"2026-06-10T10:{i // 60:02d}:{i % 60:02d},{tctl},{busy},"
                f"{sid},1000,{delta},quarantine")
        with tempfile.TemporaryDirectory() as td:
            path = make_csv(Path(td), rows)
            result = apl.analyze(path, max_lag_s=30.0, idle_busy_pct=5.0,
                                 deriv_half_window_s=2.0)
        self.assertEqual(result["coverage"]["distinct_windows"], 120)
        self.assertAlmostEqual(result["watts_idle"]["p50"], 45.0)
        lead = result["lead"]["watts_to_tctl"]
        self.assertIsNotNone(lead)
        self.assertAlmostEqual(lead["lead_s"], 12.0, delta=1.5)


if __name__ == "__main__":
    unittest.main()
