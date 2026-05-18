from __future__ import annotations

from tests.helpers import *


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
