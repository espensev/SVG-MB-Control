from __future__ import annotations

import shutil
import subprocess

from tests.helpers import *


SCRIPT = REPO_ROOT / "scripts" / "Set-EnergyLoggingProfile.ps1"
_PWSH = shutil.which("pwsh") or shutil.which("powershell")


def _run_profile(*args: str) -> subprocess.CompletedProcess:
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
class EnergyLoggingProfileTests(unittest.TestCase):
    """FEAT-0020 / REQ-PWRLOG-06 operator flip/revert workflow.

    The -DryRun path is read-only (no admin, no scheduled-task or env writes, no
    worker restart) so the workflow is testable without touching live runtime.
    """

    def test_enable_dry_run_plans_disable_revert_and_enable_energy(self) -> None:
        result = _run_profile("-Enable", "-DryRun", "-NoElevate")
        self.assertEqual(
            result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}"
        )
        out = result.stdout
        # Disables the safety-revert task so the enabled profile survives boot.
        self.assertIn("Disable-ScheduledTask", out)
        self.assertIn("SVG-MB Energy Safety Revert", out)
        # Sets the persistent worker env to enabled energy logging.
        self.assertIn("SVG_MB_CONTROL_RAPL_ENERGY_MODE", out)
        self.assertIn("'enabled'", out)
        # CPU cycles stay disabled even when energy logging is enabled.
        self.assertIn("SVG_MB_CONTROL_CPU_CYCLES_MODE", out)
        self.assertIn("'disabled'", out)
        # Dry run must not touch the live system.
        self.assertIn("dry-run", out.lower())
        # The FEAT-0006 boot-OFF-guarantee inversion is surfaced to the operator.
        self.assertIn("FEAT-0006", out)

    def test_disable_dry_run_plans_enable_revert_and_disable_energy(self) -> None:
        result = _run_profile("-Disable", "-DryRun", "-NoElevate")
        self.assertEqual(
            result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}"
        )
        out = result.stdout
        self.assertIn("Enable-ScheduledTask", out)
        self.assertIn("SVG-MB Energy Safety Revert", out)
        self.assertIn("SVG_MB_CONTROL_RAPL_ENERGY_MODE", out)
        self.assertIn("'disabled'", out)

    def test_requires_exactly_one_mode(self) -> None:
        result = _run_profile("-DryRun", "-NoElevate")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "exactly one", (result.stdout + result.stderr).lower()
        )
