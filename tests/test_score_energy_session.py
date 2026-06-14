"""Unit tests for the criterion-4 effective-frequency scoring in
scripts/score_energy_session.py (cycle validation plan, Option B).

Pure-math tests: no exe, no hardware. The script module is loaded by path
because scripts/ is not a package.
"""
from __future__ import annotations

import importlib.util
import unittest

from tests.helpers import REPO_ROOT

_SPEC = importlib.util.spec_from_file_location(
    "score_energy_session", REPO_ROOT / "scripts" / "score_energy_session.py")
ses = importlib.util.module_from_spec(_SPEC)
assert _SPEC.loader is not None
_SPEC.loader.exec_module(ses)


class EffectiveFreqVerdictTests(unittest.TestCase):
    def test_no_p0_stays_manual(self) -> None:
        # Default invocation (no Option B args) preserves the pre-change behavior:
        # report the raw ratios and stay MANUAL.
        v, detail = ses.effective_freq_verdict(1.231, 1.193)
        self.assertEqual(v, "MANUAL")
        self.assertIn("--p0-mhz", detail)
        self.assertIn("1.231", detail)

    def test_p0_without_setpoint_reports_mhz_but_manual(self) -> None:
        # A derived MHz with no reference is not validated.
        v, detail = ses.effective_freq_verdict(1.0, 1.193, p0_mhz=4300.0)
        self.assertEqual(v, "MANUAL")
        self.assertIn("derived", detail)
        self.assertIn("--locked-mhz", detail)

    def test_locked_within_tolerance_passes(self) -> None:
        # load ratio 1.0 x 4300 = 4300 MHz vs locked 4300 -> 0%.
        v, detail = ses.effective_freq_verdict(
            1.0, 1.0, p0_mhz=4300.0, locked_mhz=4300.0, tol_pct=5.0)
        self.assertEqual(v, "PASS")
        self.assertIn("locked=4300", detail)

    def test_locked_just_inside_tolerance_passes(self) -> None:
        # +4% (within +/-5%): load ratio 1.04 x 4300 = 4472 vs 4300.
        v, _ = ses.effective_freq_verdict(
            1.0, 1.04, p0_mhz=4300.0, locked_mhz=4300.0, tol_pct=5.0)
        self.assertEqual(v, "PASS")

    def test_locked_outside_tolerance_fails(self) -> None:
        # load ratio 1.3 x 4300 = 5590 vs locked 4300 -> +30% -> FAIL. This is the
        # wrong-base / unhonored-affinity failure Option B is meant to catch.
        v, detail = ses.effective_freq_verdict(
            1.0, 1.3, p0_mhz=4300.0, locked_mhz=4300.0, tol_pct=5.0)
        self.assertEqual(v, "FAIL")
        self.assertIn("+30.0%", detail)

    def test_locked_but_missing_load_ratio_is_incomplete(self) -> None:
        v, _ = ses.effective_freq_verdict(
            1.231, None, p0_mhz=4300.0, locked_mhz=4300.0)
        self.assertEqual(v, "INCOMPLETE")

    def test_uses_load_window_not_idle(self) -> None:
        # Validation is the load window (full C0 residency at the lock); a wildly
        # different idle ratio must not change the load verdict.
        v, _ = ses.effective_freq_verdict(
            0.2, 1.0, p0_mhz=4300.0, locked_mhz=4300.0, tol_pct=5.0)
        self.assertEqual(v, "PASS")


if __name__ == "__main__":
    unittest.main()
