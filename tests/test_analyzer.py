from __future__ import annotations

from tests.helpers import *


class AnalyzerToolTests(unittest.TestCase):
    def test_control_run_analyzer_writes_summary_and_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            csv_path = td / "run.csv"
            events_path = td / "events.jsonl"
            summary_path = td / "summary.md"
            manifest_path = td / "manifest.json"
            csv_path.write_text(
                "\n".join(
                    [
                        "# active_archive_path=test",
                        "cpu_tctl_c,cpu_max_c,gpu_core_c,gpu_memjn_c,gpu_hotspot_c,"
                        "loop_achieved_interval_ms,loop_work_duration_ms,loop_slip_ms,"
                        "loop_overrun,process_cpu_delta_ms,process_cpu_pct,"
                        "process_working_set_bytes,process_private_bytes,"
                        "channel0_setpoint_pct,channel0_thermal_pressure_boost_pct,"
                        "channel0_total_writes",
                        "70,72,50,60,65,50,4,0,false,2,0.1,1000,2000,50,0,1",
                        "75,76,52,62,67,50,5,0,false,2,0.2,1100,2100,55,1,2",
                        "74,75,51,61,66,60,6,10,false,2,0.2,1200,2200,54.4,1,3",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            events_path.write_text(
                "\n".join(
                    [
                        json.dumps({"event_type": "control_loop.write_applied"}),
                        json.dumps({"event_type": "control_loop.circuit_breaker_opened"}),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(REPO_ROOT / "scripts" / "analyze_control_run.py"),
                    "--csv",
                    str(csv_path),
                    "--events",
                    str(events_path),
                    "--out",
                    str(summary_path),
                    "--manifest-out",
                    str(manifest_path),
                    "--profile",
                    "smoke",
                ],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
            )

            self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
            summary = summary_path.read_text(encoding="utf-8")
            self.assertIn("SVG-MB-Control Run Summary", summary)
            self.assertIn("control_loop.write_applied", summary)
            self.assertIn("control_loop.circuit_breaker_opened", summary)

            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(manifest["schema"], "svg_mb_control.analysis_manifest.v1")
            self.assertEqual(manifest["row_count"], 3)
            self.assertEqual(manifest["event_count"], 2)
            self.assertTrue(manifest["artifacts"]["csv"]["sha256"])
