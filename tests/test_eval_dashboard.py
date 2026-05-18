from __future__ import annotations

import importlib.util

from tests.helpers import *


def _load_dashboard_server():
    path = REPO_ROOT / "tools" / "eval_dashboard" / "server.py"
    spec = importlib.util.spec_from_file_location("eval_dashboard_server", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class EvalDashboardTests(unittest.TestCase):
    def test_static_dashboard_assets_are_wired(self) -> None:
        dashboard = REPO_ROOT / "tools" / "eval_dashboard"
        html = (dashboard / "index.html").read_text(encoding="utf-8")
        js = (dashboard / "dashboard.js").read_text(encoding="utf-8")
        css = (dashboard / "styles.css").read_text(encoding="utf-8")

        self.assertIn('id="csvFile"', html)
        self.assertIn('id="cpuThreshold"', html)
        self.assertIn('value="65"', html)
        self.assertIn('id="temperatureChart"', html)
        self.assertIn('src="dashboard.js"', html)
        self.assertIn('href="styles.css"', html)
        self.assertIn("function parseCsv", js)
        self.assertIn("function gpuEnvelope", js)
        self.assertIn("function timeAtOrAbove", js)
        self.assertIn("function loadLiveRun", js)
        self.assertIn("/api/live-tail.csv", js)
        self.assertNotIn("n/a", html.lower())
        self.assertNotIn("n/a", js.lower())
        self.assertIn(".metric-grid", css)

        # Runtime Health panel (Runtime Robustness Step 4)
        self.assertIn('id="healthState"', html)
        self.assertIn('id="healthDetail"', html)
        self.assertIn("Runtime Health", html)
        self.assertIn("function summarizeHealth", js)
        self.assertIn("function loadHealth", js)
        self.assertIn("/api/health.json", js)
        self.assertIn(".health-pill", css)

    def test_build_health_payload_aggregates_sidecars(self) -> None:
        server = _load_dashboard_server()
        with tempfile.TemporaryDirectory() as td_str:
            base = Path(td_str) / "release" / "runtime"
            base.mkdir(parents=True, exist_ok=True)
            _write_json(
                base / "control_health.json",
                {
                    "schema_version": 1,
                    "last_health_state": "healthy",
                    "last_health_reason": "fresh",
                    "last_health_time": "2026-05-18T15:00:00",
                },
            )
            _write_json(
                base / "control_supervisor.json",
                {
                    "schema_version": 1,
                    "supervisor_pid": 111,
                    "worker_restart_count": 2,
                    "last_worker_exit_code": 0,
                },
            )
            _write_json(
                base / "control_runtime.json",
                {
                    "schema_version": 4,
                    "mode": "control-loop",
                    "status": "running",
                    "process_id": 222,
                    "loop_last_evaluation": "2026-05-18T15:00:01",
                    "last_successful_restore_time": "2026-05-18T14:58:00",
                },
            )

            payload = server.build_health_payload(Path(td_str))

            self.assertTrue(payload["available"])
            self.assertEqual(payload["health"]["last_health_state"], "healthy")
            self.assertEqual(payload["supervisor"]["worker_restart_count"], 2)
            self.assertEqual(payload["runtime"]["mode"], "control-loop")
            self.assertEqual(
                payload["runtime"]["last_successful_restore_time"],
                "2026-05-18T14:58:00",
            )
            self.assertNotIn("schema_version", payload["runtime"])

    def test_build_health_payload_tolerates_missing_files(self) -> None:
        server = _load_dashboard_server()
        with tempfile.TemporaryDirectory() as td_str:
            payload = server.build_health_payload(Path(td_str))
            self.assertFalse(payload["available"])
            self.assertIsNone(payload["health"])
            self.assertIsNone(payload["supervisor"])
            self.assertIsNone(payload["runtime"])

    def test_dashboard_server_help(self) -> None:
        result = subprocess.run(
            [
                "powershell",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(REPO_ROOT / "scripts" / "Start-EvalDashboard.ps1"),
                "-Help",
            ],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
        self.assertIn("Serve the local SVG-MB-Control eval dashboard", result.stdout)
        self.assertIn("serves the repo root", result.stdout)
