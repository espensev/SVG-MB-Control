"""Unit tests for the cycle-column selection in scripts/cpu_config_fingerprint.py
(all-core preference). Pure: no exe, no hardware. The script module is loaded by
path because scripts/ is not a package.
"""
from __future__ import annotations

import importlib.util
import sys
import unittest

from tests.helpers import REPO_ROOT

_SPEC = importlib.util.spec_from_file_location(
    "cpu_config_fingerprint",
    REPO_ROOT / "scripts" / "cpu_config_fingerprint.py")
fp = importlib.util.module_from_spec(_SPEC)
assert _SPEC.loader is not None
# Register before exec: the module's @dataclass machinery resolves field types
# via sys.modules[cls.__module__], which is None unless the module is registered.
sys.modules["cpu_config_fingerprint"] = fp
_SPEC.loader.exec_module(fp)

_ALLCORE_HEADER = [
    "cpu_cycles_sample_id", "cpu_aperf_delta", "cpu_mperf_delta",
    "cpu_cycles_allcore_sample_id", "cpu_aperf_delta_allcore",
    "cpu_mperf_delta_allcore",
]


class SelectCycleColumnsTests(unittest.TestCase):
    """Prefer the all-core package columns (and their own sample id, which the
    window de-dup keys on) when present; fall back to the per-core (core-0)
    columns so captures predating the all-core rollup still fingerprint."""

    def test_prefers_allcore_when_present(self) -> None:
        rows = [["1", "100", "80", "7", "3200", "2000"],
                ["1", "100", "80", "7", "3200", "2000"]]
        ids, aperf, mperf = fp._select_cycle_columns(_ALLCORE_HEADER, rows)
        self.assertEqual(ids, ["7", "7"])           # all-core sample id, not "1"
        self.assertEqual(aperf, [3200.0, 3200.0])
        self.assertEqual(mperf, [2000.0, 2000.0])

    def test_falls_back_to_core0_when_allcore_absent(self) -> None:
        header = ["cpu_cycles_sample_id", "cpu_aperf_delta", "cpu_mperf_delta"]
        rows = [["1", "100", "80"], ["1", "100", "80"]]
        ids, aperf, mperf = fp._select_cycle_columns(header, rows)
        self.assertEqual(ids, ["1", "1"])
        self.assertEqual(aperf, [100.0, 100.0])
        self.assertEqual(mperf, [80.0, 80.0])

    def test_falls_back_when_allcore_present_but_blank(self) -> None:
        # The sweeper ran but every sweep was baseline/blanked -> all-core empty.
        rows = [["1", "100", "80", "", "", ""],
                ["1", "100", "80", "", "", ""]]
        ids, aperf, mperf = fp._select_cycle_columns(_ALLCORE_HEADER, rows)
        self.assertEqual(ids, ["1", "1"])
        self.assertEqual(aperf, [100.0, 100.0])


if __name__ == "__main__":
    unittest.main()
