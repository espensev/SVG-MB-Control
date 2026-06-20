"""Unit tests for scripts/analyze_cpu_temp_power.py."""
from __future__ import annotations

import csv
import importlib.util
import sys
import tempfile
import unittest
from datetime import datetime, timedelta
from pathlib import Path

from tests.helpers import REPO_ROOT

_SPEC = importlib.util.spec_from_file_location(
    "analyze_cpu_temp_power",
    REPO_ROOT / "scripts" / "analyze_cpu_temp_power.py")
ctp = importlib.util.module_from_spec(_SPEC)
assert _SPEC.loader is not None
sys.modules[_SPEC.name] = ctp
_SPEC.loader.exec_module(ctp)

_BASE = datetime(2026, 6, 20, 12, 0, 0)


def _wall(i: int) -> str:
    return (_BASE + timedelta(seconds=i)).strftime("%Y-%m-%dT%H:%M:%S")


def _values(i: int) -> tuple[float, float, float, float, float]:
    if i < 5:
        return 45.0, 42.0, 4.0, 40.0, 50_000.0
    if i < 15:
        return 90.0, 55.0 + min(i - 5, 4) * 0.5, 35.0, 45.0, 100_000.0
    if i < 25:
        return 130.0, 66.0 + min(i - 15, 4) * 0.4, 80.0, 50.0, 120_000.0
    return 140.0, 74.0, 82.0, 65.0, 350_000.0


def _make_csv(path: Path, rows: int = 35) -> Path:
    header = [
        "wall_clock", "cpu_tctl_c", "cpu_max_c", "system_cpu_busy_pct",
        "cpu_power_sample_id", "cpu_power_window_ms",
        "cpu_pkg_energy_delta_uj", "cpu_pkg_energy_acquisition",
        "gpu_memjn_c", "gpu_power_mw",
    ]
    for channel in (0, 1, 2, 3, 4, 5):
        header.extend([
            f"channel{channel}_setpoint_pct",
            f"fan{channel}_rpm",
        ])
    rows_out: list[list[object]] = []
    for i in range(rows):
        watts, tctl, busy, gpu_mem, gpu_mw = _values(i)
        delta_uj = int(watts * 1000 * 1000)
        rad_step = 0.05 * max(0, i - 5)
        row: list[object] = [
            _wall(i), tctl, tctl + 0.5, busy, i + 1, 1000, delta_uj,
            "quarantine", gpu_mem, gpu_mw,
        ]
        for channel in (0, 1, 2, 3, 4, 5):
            if channel in (1, 4, 5):
                row.extend([30.0 + rad_step, 900 + i])
            else:
                row.extend([35.0, 700 + i])
        rows_out.append(row)
    with path.open("w", newline="", encoding="utf-8") as fh:
        fh.write("# schema=svg_mb_control.log.v1\n")
        fh.write("# mode=control-loop\n")
        fh.write("# session_start=2026-06-20T12:00:00\n")
        writer = csv.writer(fh)
        writer.writerow(header)
        writer.writerows(rows_out)
    return path


class CpuTempPowerTests(unittest.TestCase):
    def test_loads_radiator_channels_from_machine_policy(self) -> None:
        policy = ctp.load_channel_policy(
            REPO_ROOT / "config" / "machines" / "snd-desk.cooling.policy.json")
        self.assertTrue(policy["loaded"])
        self.assertEqual(policy["radiator_channels"], [1, 4, 5])
        self.assertEqual(policy["context_channels"], [0, 2, 3])
        self.assertEqual(policy["excluded_channels"], [6])

    def test_power_bands_and_gpu_confound_split(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = _make_csv(Path(td) / "control.csv")
            result = ctp.analyze(
                path,
                ambient_c=21.0,
                bands=ctp.parse_power_bands("idle:0:60,mid:60:120,high:120:"),
                dwell_s=3.0,
                max_gap_s=2.0,
                gpu_power_confound_w=300.0,
                gpu_mem_confound_c=60.0,
            )
            windows = result.pop("_windows")

        self.assertEqual(result["windows"], 35)
        self.assertEqual(len(windows), 35)
        self.assertEqual(result["acquisition_windows"], {"quarantine": 35})

        high = result["bands"]["high"]
        self.assertGreater(high["gate"]["settled"], 0)
        self.assertGreater(high["all_settled"]["windows"],
                           high["gpu_unconfounded_settled"]["windows"])
        self.assertGreater(
            high["all_settled"]["theta_c_per_w"]["p50"],
            0.30,
        )

        markdown = ctp.render_markdown(result)
        self.assertIn("CPU temperature by package power", markdown)
        self.assertIn("theta C/W", markdown)

    def test_missing_power_columns_reports_warning(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "control.csv"
            path.write_text(
                "# schema=svg_mb_control.log.v1\n"
                "wall_clock,cpu_tctl_c\n"
                "2026-06-20T12:00:00,45\n",
                encoding="utf-8")
            result = ctp.analyze(
                path,
                ambient_c=21.0,
                bands=ctp.parse_power_bands("idle:0:60"),
                dwell_s=3.0,
                max_gap_s=2.0,
                gpu_power_confound_w=300.0,
                gpu_mem_confound_c=60.0,
            )
            result.pop("_windows")

        self.assertEqual(result["windows"], 0)
        self.assertIn("missing power evaluation", result["warning"])


if __name__ == "__main__":
    unittest.main()
