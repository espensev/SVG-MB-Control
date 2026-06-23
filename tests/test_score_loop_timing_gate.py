"""Unit tests for scripts/score_loop_timing_gate.py -- the FEAT-0006 section-12
off-thread-sweeper loop-timing gate. It compares loop_work_duration_ms between a
cycles-OFF baseline and a candidate (the FEAT-0020 gate-6 analog), bucketed by
GPU load to control the documented environmental-spike confound. The
discriminator is p99-of-bulk (sustained per-tick cost); max + overrun count are
context only, so a single multi-second environmental stall never fails the gate.

Pure-math: no exe, no hardware. Module loaded by path (scripts/ is not a
package); registered in sys.modules so any dataclass field types resolve.
"""
from __future__ import annotations

import importlib.util
import sys
import unittest

from tests.helpers import REPO_ROOT

_SPEC = importlib.util.spec_from_file_location(
    "score_loop_timing_gate",
    REPO_ROOT / "scripts" / "score_loop_timing_gate.py")
g = importlib.util.module_from_spec(_SPEC)
assert _SPEC.loader is not None
sys.modules["score_loop_timing_gate"] = g
_SPEC.loader.exec_module(g)


class PercentileTests(unittest.TestCase):
    def test_basic_median(self) -> None:
        self.assertEqual(g._percentile([1, 2, 3, 4, 5], 50), 3)

    def test_empty_is_none(self) -> None:
        self.assertIsNone(g._percentile([], 50))

    def test_single_value(self) -> None:
        self.assertEqual(g._percentile([7.0], 99), 7.0)


class GpuBucketTests(unittest.TestCase):
    def test_buckets_by_milliwatts(self) -> None:
        self.assertEqual(g.gpu_bucket(None), "unknown")
        self.assertEqual(g.gpu_bucket(50_000.0), "idle")    # 50 W
        self.assertEqual(g.gpu_bucket(200_000.0), "mid")    # 200 W
        self.assertEqual(g.gpu_bucket(400_000.0), "load")   # 400 W
        # Boundaries: idle < 150 W <= mid < 350 W <= load.
        self.assertEqual(g.gpu_bucket(150_000.0), "mid")
        self.assertEqual(g.gpu_bucket(350_000.0), "load")


class TimingStatsTests(unittest.TestCase):
    def test_environmental_spike_isolated_from_bulk(self) -> None:
        # 99 normal ~3 ms ticks + one 3000 ms environmental spike. max reflects
        # the spike and it counts as 1 spike / 1 overrun, but p99_bulk excludes
        # it so a single documented background stall cannot dominate the gate.
        work = [3.0] * 99 + [3000.0]
        s = g.timing_stats(work, stall_ms=100.0, overrun_ms=250.0)
        self.assertEqual(s["n"], 100)
        self.assertEqual(s["max"], 3000.0)
        self.assertEqual(s["spikes"], 1)
        self.assertEqual(s["overruns"], 1)
        self.assertEqual(s["p99_bulk"], 3.0)

    def test_ignores_none_values(self) -> None:
        s = g.timing_stats([1.0, None, 2.0, None, 3.0], stall_ms=100.0)
        self.assertEqual(s["n"], 3)

    def test_empty_is_unavailable(self) -> None:
        s = g.timing_stats([], stall_ms=100.0)
        self.assertEqual(s["n"], 0)
        self.assertIsNone(s["p99_bulk"])
        self.assertIsNone(s["max"])


class CompareBucketTests(unittest.TestCase):
    def _stats(self, work):
        return g.timing_stats(work, stall_ms=100.0, overrun_ms=250.0)

    def test_within_tolerance_passes(self) -> None:
        ok, _ = g.compare_bucket(self._stats([3.0] * 100),
                                 self._stats([3.2] * 100))
        self.assertTrue(ok)

    def test_single_candidate_spike_does_not_fail(self) -> None:
        # Candidate adds ONE environmental spike (huge max, 1 overrun) but an
        # identical bulk -> must still pass: max/overruns are context, not the
        # discriminator (advisor: never fail the gate on a single spike).
        ok, reason = g.compare_bucket(self._stats([3.0] * 100),
                                      self._stats([3.0] * 99 + [3000.0]))
        self.assertTrue(ok, reason)

    def test_raised_bulk_p99_fails(self) -> None:
        # The candidate's BULK p99 jumps ~10x (sustained per-tick cost, the
        # sweeper's expected signature) -> the real signal must fail.
        ok, _ = g.compare_bucket(self._stats([3.0] * 100),
                                 self._stats([30.0] * 100))
        self.assertFalse(ok)

    def test_insufficient_data_is_inconclusive(self) -> None:
        ok, _ = g.compare_bucket(self._stats([]), self._stats([3.0] * 100))
        self.assertIsNone(ok)


if __name__ == "__main__":
    unittest.main()
