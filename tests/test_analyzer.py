from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from tests.helpers import REPO_ROOT, _ensure_release_build

ANALYZER = REPO_ROOT / "scripts" / "analyze_control_run.py"

# Minimal raw control-loop CSV: 3 ticks with a rising GPU envelope (derived
# from gpu_core/memjn/hotspot) and a low-band boost on tick 2. ParseControlLoopCsv
# requires wall_clock + loop_tick_count; other columns are optional.
CSV_HEADER = ",".join(
    [
        "loop_tick_count",
        "wall_clock",
        "cpu_tctl_c",
        "gpu_core_c",
        "gpu_memjn_c",
        "gpu_hotspot_c",
        "loop_achieved_interval_ms",
        "loop_work_duration_ms",
        "loop_slip_ms",
        "loop_overrun",
        "process_cpu_pct",
        "process_working_set_bytes",
        "process_private_bytes",
        "channel0_observed_temp_c",
        "channel0_setpoint_pct",
        "channel0_thermal_pressure_boost_pct",
        "channel0_midband_pressure_boost_pct",
        "channel0_gpu_airflow_boost_pct",
        "channel0_cpu_low_soak_boost_pct",
        "channel0_low_band_effective_boost_pct",
        "channel0_response_source",
        "channel0_write_reason",
        "channel0_total_writes",
    ]
)
CSV_ROWS = [
    # envelope 65, no boosts
    "1,2026-05-15T03:30:00,70,50,60,65,50,10,0,false,0.1,1000,2000,"
    "70,50,0,0,0,0,0,primary_curve,first_write,1",
    # envelope 67 (peak); boost total = midband 1.0 + low_band 2.0 = 3.0
    "2,2026-05-15T03:30:01,75,52,62,67,50,10,0,false,0.2,1100,2100,"
    "75,55,0,1.0,0,0,2.0,primary_curve,setpoint_delta,2",
    # envelope 66
    "3,2026-05-15T03:30:02,74,51,61,66,60,10,0,false,0.2,1200,2200,"
    "74,54.4,0,0,0,0,0,primary_curve,setpoint_delta,3",
]
CSV_TEXT = CSV_HEADER + "\n" + "\n".join(CSV_ROWS) + "\n"


def _run_wrapper(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(ANALYZER), *args],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
    )


class AnalyzerWrapperTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if sys.platform != "win32":
            raise unittest.SkipTest("svg-mb-control.exe is Windows-only")
        _ensure_release_build()

    def _write_csv(self, td: Path) -> Path:
        csv_path = td / "run.csv"
        csv_path.write_text(CSV_TEXT, encoding="utf-8")
        return csv_path

    def test_json_report_via_wrapper(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            csv_path = self._write_csv(Path(td_str))
            result = _run_wrapper(
                "--csv", str(csv_path),
                "--gpu-load-threshold-c", "66",
                "--format", "json",
            )
            self.assertEqual(
                result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}"
            )
            obj = json.loads(result.stdout)
            gpu = obj["gpu_response"]
            self.assertEqual(gpu["peak"]["value_c"], 67.0)
            self.assertEqual(gpu["peak"]["row_number"], 2)
            self.assertEqual(gpu["above_threshold_rows"], 2)
            self.assertEqual(
                gpu["channels"]["0"]["setpoint_at_peak_pct"], 55.0
            )
            channels = {c["channel"]: c for c in obj["channels"]}
            self.assertEqual(channels[0]["response_boost_total_pct"], 3.0)
            # Timing/resource sections are present and reflect the constant rows.
            self.assertEqual(
                obj["timing"]["loop_achieved_interval_ms"]["max"], 60.0
            )
            self.assertEqual(obj["timing"]["loop_work_duration_ms"]["max"], 10.0)
            self.assertEqual(obj["timing"]["overrun_count"], 0)

    def test_markdown_report_via_wrapper(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            csv_path = self._write_csv(Path(td_str))
            result = _run_wrapper("--csv", str(csv_path))
            self.assertEqual(
                result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}"
            )
            self.assertIn("analyze report:", result.stdout)
            self.assertIn("gpu_response:", result.stdout)
            self.assertIn("timing:", result.stdout)
            self.assertIn("resources:", result.stdout)

    def test_wrapper_help_has_no_native_owned_flags(self) -> None:
        result = _run_wrapper("--help")
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("--csv", result.stdout)
        self.assertIn("--gpu-load-threshold-c", result.stdout)
        for removed in (
            "--manifest-out",
            "--decision-record-out",
            "--hypothesis",
            "--cpu-load-threshold-c",
            "--low-response-setpoint-pct",
        ):
            self.assertNotIn(removed, result.stdout)
