from __future__ import annotations

import json
import shutil
import subprocess
import tempfile
from pathlib import Path

from tests.helpers import *


SCRIPT = REPO_ROOT / "Set-SVG-MB-ControlRuntimeWindow.ps1"
_PWSH = shutil.which("pwsh") or shutil.which("powershell")


def _run_window(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            _PWSH,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(SCRIPT),
            *args,
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
    )


@unittest.skipUnless(_PWSH, "no PowerShell host (powershell/pwsh) on PATH")
class RuntimeWindowScriptTests(unittest.TestCase):
    """FEAT-0026 operator runtime-window helper dry-run coverage."""

    def test_pause_dry_run_disables_tasks_and_schedules_resume_with_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            exe = td / "svg-mb-control.exe"
            config = td / "control.json"
            state = td / "active_window.json"
            result = _run_window(
                "-Pause",
                "-For",
                "1h",
                "-EvidenceLog",
                "-DryRun",
                "-NoElevate",
                "-ExePath",
                str(exe),
                "-ConfigPath",
                str(config),
                "-StatePath",
                str(state),
            )

        self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
        out = result.stdout
        self.assertIn("[dry-run] write operator window state", out)
        self.assertIn("disable scheduled task \\SVG-MB Control\\SVG-MB Control Watchdog", out)
        self.assertIn("disable scheduled task \\SVG-MB Control\\SVG-MB Control", out)
        self.assertIn("--stop --config", out)
        self.assertIn("register one-shot resume task", out)
        self.assertIn("register evidence-log task", out)
        self.assertIn("--mode evidence-log --config", out)
        self.assertIn("Window active until:", out)

    def test_pause_requires_a_bounded_resume_time(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            result = _run_window(
                "-Pause",
                "-DryRun",
                "-NoElevate",
                "-ExePath",
                str(td / "svg-mb-control.exe"),
                "-ConfigPath",
                str(td / "control.json"),
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("-Pause requires", result.stdout + result.stderr)

    def test_restart_dry_run_uses_packaged_restart_path(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            result = _run_window(
                "-Restart",
                "-DryRun",
                "-NoElevate",
                "-ExePath",
                str(td / "svg-mb-control.exe"),
                "-ConfigPath",
                str(td / "control.json"),
            )

        self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
        self.assertIn("--restart --config", result.stdout)

    def test_status_json_dry_run_is_machine_readable_for_coordinators(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            result = _run_window(
                "-Status",
                "-Json",
                "-DryRun",
                "-NoElevate",
                "-ExePath",
                str(td / "svg-mb-control.exe"),
                "-ConfigPath",
                str(td / "control.json"),
                "-StatePath",
                str(td / "active_window.json"),
            )

        self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
        payload = json.loads(result.stdout)
        self.assertEqual(payload["schema_version"], 1)
        self.assertEqual(payload["helper"], "Set-SVG-MB-ControlRuntimeWindow.ps1")
        self.assertEqual(payload["coordinator_contract"], "process-boundary")
        self.assertFalse(payload["sibling_repo_dependency"])
        self.assertIsNone(payload["active_window"])
        self.assertIn("control", payload["tasks"])
        self.assertIn("watchdog", payload["tasks"])


if __name__ == "__main__":
    unittest.main()
