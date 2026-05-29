from __future__ import annotations

import importlib.util

from tests.helpers import *

ANALYZER = REPO_ROOT / "scripts" / "analyze_control_run.py"

CSV_TEXT = (
    "\n".join(
        [
            "# active_archive_path=test",
            "cpu_tctl_c,cpu_max_c,gpu_core_c,gpu_memjn_c,gpu_hotspot_c,"
            "loop_achieved_interval_ms,loop_work_duration_ms,loop_slip_ms,"
            "loop_overrun,process_cpu_delta_ms,process_cpu_pct,"
            "process_working_set_bytes,process_private_bytes,"
            "channel0_setpoint_pct,channel0_thermal_pressure_boost_pct,"
            "channel0_response_source,channel0_write_reason,"
            "channel0_total_writes",
            "70,72,50,60,65,50,4,0,false,2,0.1,1000,2000,50,0,"
            "primary_curve,first_write,1",
            "75,76,52,62,67,50,5,0,false,2,0.2,1100,2100,55,1,"
            "primary_curve,setpoint_delta,2",
            "74,75,51,61,66,60,6,10,false,2,0.2,1200,2200,54.4,1,"
            "primary_curve,setpoint_delta,3",
        ]
    )
    + "\n"
)

EVENTS_TEXT = (
    "\n".join(
        [
            json.dumps(
                {
                    "event_type": "control_loop.write_applied",
                    "severity": "info",
                    "error_code": "none",
                }
            ),
            json.dumps(
                {
                    "event_type": "control_loop.circuit_breaker_opened",
                    "severity": "warning",
                    "error_code": "CONTROL_LOOP_CIRCUIT_BREAKER_OPENED",
                }
            ),
        ]
    )
    + "\n"
)


def _load_analyzer():
    spec = importlib.util.spec_from_file_location("analyze_control_run", ANALYZER)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class AnalyzerToolTests(unittest.TestCase):
    def test_raw_csv_summary_markdown(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            csv_path = td / "run.csv"
            events_path = td / "events.jsonl"
            summary_path = td / "summary.md"
            # The narrowed analyzer no longer writes a decision record beside
            # the Markdown summary; assert that path stays absent.
            decision_path = td / "summary.decision.md"
            csv_path.write_text(CSV_TEXT, encoding="utf-8")
            events_path.write_text(EVENTS_TEXT, encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    str(ANALYZER),
                    "--csv",
                    str(csv_path),
                    "--events",
                    str(events_path),
                    "--out",
                    str(summary_path),
                    "--profile",
                    "smoke",
                    "--gpu-load-threshold-c",
                    "66",
                ],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
            )

            self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
            summary = summary_path.read_text(encoding="utf-8")
            self.assertIn("SVG-MB-Control Run Summary", summary)
            self.assertIn("Profile: smoke", summary)
            self.assertIn("gpu_envelope_c", summary)
            self.assertIn("GPU envelope peak: 67.00 C", summary)
            self.assertIn("rows above/equal: 2", summary)
            self.assertIn("control_loop.write_applied", summary)
            self.assertIn("control_loop.circuit_breaker_opened", summary)
            self.assertIn("Event severity: info:1, warning:1", summary)
            self.assertIn("CONTROL_LOOP_CIRCUIT_BREAKER_OPENED:1", summary)
            self.assertFalse(decision_path.exists())

    def test_raw_csv_summary_json(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            csv_path = td / "run.csv"
            json_path = td / "summary.json"
            csv_path.write_text(CSV_TEXT, encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    str(ANALYZER),
                    "--csv",
                    str(csv_path),
                    "--format",
                    "json",
                    "--out",
                    str(json_path),
                    "--gpu-load-threshold-c",
                    "66",
                ],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}"
            )
            summary_json = json.loads(json_path.read_text(encoding="utf-8"))
            self.assertEqual(
                summary_json["temperatures"]["gpu_envelope_c"]["max"], 67.0
            )
            self.assertEqual(summary_json["gpu_response"]["peak"]["row_number"], 2)
            self.assertEqual(summary_json["gpu_response"]["above_threshold_rows"], 2)
            self.assertEqual(
                summary_json["gpu_response"]["channels"]["0"]["setpoint_at_peak_pct"],
                55.0,
            )

    def test_native_owned_flags_removed(self) -> None:
        # The decision-record / manifest / diagnostic-threshold surface is now
        # native-owned; assert those flags are gone from this raw-CSV fallback.
        result = subprocess.run(
            [sys.executable, str(ANALYZER), "--help"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
        for removed in (
            "--manifest-out",
            "--decision-record-out",
            "--no-decision-record",
            "--decision ",
            "--hypothesis",
            "--cpu-load-threshold-c",
            "--low-response-setpoint-pct",
        ):
            self.assertNotIn(removed, result.stdout)

    def test_percentile_is_nearest_rank(self) -> None:
        # Matches the native report (src/analyze/analyze_report_data.cpp): p90
        # of {1..5} is 5.0 (round(3.6) -> index 4), not the 4.6 a linear
        # interpolation would give. Keeps the two reports byte-comparable.
        analyzer = _load_analyzer()
        self.assertEqual(analyzer.percentile([1, 2, 3, 4, 5], 90.0), 5.0)
        self.assertEqual(analyzer.percentile([1, 2, 3, 4, 5], 50.0), 3.0)
        self.assertEqual(analyzer.percentile([1, 2, 3, 4, 5], 100.0), 5.0)
        self.assertEqual(analyzer.percentile([10.0], 90.0), 10.0)
        self.assertIsNone(analyzer.percentile([], 90.0))
