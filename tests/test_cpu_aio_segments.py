"""Unit tests for scripts/extract_cpu_aio_segments.py.

The extractor is offline/read-only and loaded by path because scripts/ is not a
package.
"""
from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from datetime import datetime, timedelta
from pathlib import Path

from tests.helpers import REPO_ROOT

_SPEC = importlib.util.spec_from_file_location(
    "extract_cpu_aio_segments",
    REPO_ROOT / "scripts" / "extract_cpu_aio_segments.py")
cas = importlib.util.module_from_spec(_SPEC)
assert _SPEC.loader is not None
_SPEC.loader.exec_module(cas)

_BASE = datetime(2026, 6, 18, 12, 0, 0)
_FMT = "%Y-%m-%dT%H:%M:%S"


def _wall(i: int) -> str:
    return (_BASE + timedelta(seconds=i)).strftime(_FMT)


def _row(i: int) -> str:
    if i < 10:
        cpu_t, watts, busy = 55.0, 50.0, 5.0
    elif i == 10:
        cpu_t, watts, busy = 65.0, 90.0, 65.0
    elif i == 11:
        cpu_t, watts, busy = 73.0, 130.0, 95.0
    else:
        cpu_t = max(58.0, 73.0 - (i - 11) * 0.7)
        watts = 95.0 if i < 25 else 70.0
        busy = 70.0 if i < 25 else 20.0
    gpu_mem = 45.0 if i < 25 else 62.0
    gpu_mw = 100_000 if i < 25 else 350_000
    step = max(0, i - 10)
    channels = {
        0: (15.5, 15.5, 600, "primary_curve"),
        1: (22.0 + min(step * 0.20, 1.20), 22.0 + min(step * 0.20, 1.20),
            500 + min(step * 8, 48), "cpu_override"),
        2: (50.0 + min(step * 0.80, 5.00), 50.0 + min(step * 0.80, 5.00),
            700 + min(step * 12, 96), "cpu_override"),
        3: (46.0 + min(step * 0.80, 5.00), 46.0 + min(step * 0.80, 5.00),
            680 + min(step * 12, 96), "cpu_override"),
        4: (29.0 + min(step * 0.50, 3.00), 29.0 + min(step * 0.50, 3.00),
            1050 + min(step * 22, 132), "cpu_override"),
        5: (20.0 + min(step * 0.30, 2.00), 20.0 + min(step * 0.30, 2.00),
            730 + min(step * 14, 84), "cpu_override"),
    }
    delta_uj = int(watts * 1_000_000)  # 1000 ms window -> W.
    values: list[object] = [
        _wall(i), i + 1, cpu_t, cpu_t, busy, i + 1, 1000, delta_uj,
        "quarantine", gpu_mem, gpu_mw,
    ]
    for channel in range(6):
        setpoint, duty, rpm, source = channels[channel]
        values.extend([setpoint, duty, rpm, source])
    return ",".join(str(v) for v in values)


def _make_csv(tmp: Path) -> Path:
    header = [
        "wall_clock", "loop_tick_count", "cpu_tctl_c", "cpu_max_c",
        "system_cpu_busy_pct", "cpu_power_sample_id", "cpu_power_window_ms",
        "cpu_pkg_energy_delta_uj", "cpu_pkg_energy_acquisition",
        "gpu_memjn_c", "gpu_power_mw",
    ]
    for channel in range(6):
        header.extend([
            f"channel{channel}_setpoint_pct",
            f"channel{channel}_duty_pct",
            f"channel{channel}_rpm",
            f"channel{channel}_response_source",
        ])
    path = tmp / "control.csv"
    path.write_text(
        "# schema=svg_mb_control.log.v1\n# mode=control-loop\n"
        "# session_start=2026-06-18T12:00:00\n"
        + ",".join(header) + "\n"
        + "\n".join(_row(i) for i in range(40)) + "\n",
        encoding="utf-8")
    return path


class CpuAioSegmentTests(unittest.TestCase):
    def _analyze(self, path: Path) -> tuple[dict, dict]:
        summary = cas.analyze(
            path,
            cpu_tctl_thresholds_c=[64.0, 72.0],
            cpu_power_thresholds_w=[80.0, 120.0],
            pre_s=5.0,
            post_s=25.0,
            merge_events_within_s=60.0,
            gpu_power_confound_w=300.0,
            gpu_mem_confound_c=60.0,
            max_segments=10,
        )
        data = summary.pop("_data")
        return summary, data

    def test_clusters_cpu_power_and_tctl_into_one_radiator_segment(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            summary, _ = self._analyze(_make_csv(Path(td)))
        self.assertEqual(len(summary["segments"]), 1)
        segment = summary["segments"][0]
        self.assertEqual(segment["event"]["kind"], "cpu_tctl_c")
        self.assertEqual(segment["event"]["threshold"], 64.0)
        self.assertGreaterEqual(len(segment["event"]["cluster_reasons"]), 4)
        self.assertTrue(segment["context"]["gpu_confounded"])
        self.assertAlmostEqual(segment["context"]["first_gpu_confound_s"], 15.0)

        rad = segment["radiator_group"]
        self.assertAlmostEqual(rad["setpoint_delta_max_pct"], 2.067, places=3)
        self.assertAlmostEqual(rad["times_to_setpoint_delta_s"]["1.0"], 3.0)
        self.assertIsNone(rad["times_to_setpoint_delta_s"]["3.0"])
        self.assertAlmostEqual(
            segment["context_fans"]["setpoint_delta_max_pct"], 3.333, places=3)

        self.assertAlmostEqual(
            segment["channels"]["4"]["setpoint_delta_max_pct"], 3.0)
        self.assertAlmostEqual(
            segment["channels"]["1"]["setpoint_delta_max_pct"], 1.2)
        self.assertEqual(
            segment["channels"]["5"]["response_source_counts"]["cpu_override"],
            15)
        self.assertEqual(segment["analysis_window"]["ended_by"], "gpu_confound")

    def test_writes_index_and_compact_segment_trace(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            summary, data = self._analyze(_make_csv(root))
            outputs = cas.write_outputs(root / "out", summary, data,
                                        write_segments=True)

            payload = json.loads(Path(outputs["json"]).read_text(encoding="utf-8"))
            self.assertNotIn("_data", payload)
            self.assertEqual(payload["params"]["radiator_channels"], [1, 4, 5])

            index_lines = Path(outputs["csv"]).read_text(
                encoding="utf-8").splitlines()
            self.assertIn("rad_setpoint_delta_max_pct", index_lines[0])
            self.assertEqual(len(index_lines), 2)

            trace = Path(outputs["segment_traces"]) / "segment-01.csv"
            trace_lines = trace.read_text(encoding="utf-8").splitlines()
            self.assertIn("rad_setpoint_avg_pct", trace_lines[0])
            self.assertGreater(len(trace_lines), 10)


if __name__ == "__main__":
    unittest.main()
