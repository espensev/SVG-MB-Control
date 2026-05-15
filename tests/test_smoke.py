from __future__ import annotations

import re

from tests.helpers import *


class SmokeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if sys.platform != "win32":
            raise unittest.SkipTest("Windows-only repo")
        _ensure_release_build()

    def test_direct_one_shot_outputs_json(self) -> None:
        result = _run_control(
            "--mode",
            "one-shot",
            env=_sim_direct_env(channel=0, amd_temp_c=81.5, duty_raw=128, mode_raw=5),
        )
        self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
        data = json.loads(result.stdout)
        self.assertEqual(data["amd_sensors"][0]["label"], "Tctl/Tdie")
        self.assertEqual(data["amd_sensors"][0]["temperature_c"], 81.5)
        self.assertEqual(data["fans"][0]["channel"], 0)
        self.assertEqual(data["fans"][0]["duty_raw"], 128)
        self.assertEqual(data["fans"][0]["mode_raw"], 5)

    def test_diagnose_amd_sim_mode_outputs_snapshot(self) -> None:
        result = _run_control(
            "--diagnose-amd",
            env={
                "SVG_MB_CONTROL_SIM_DIRECT_AMD_MODE": "enabled",
                "SVG_MB_CONTROL_SIM_AMD_TCTL_C": "82.5",
            },
        )
        self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
        self.assertIn("amd_reader.available: true", result.stdout)
        self.assertIn('sample.available: true', result.stdout)
        self.assertIn('sample[0].label: "Tctl/Tdie"', result.stdout)
        self.assertIn("sample[0].temperature_c: 82.5", result.stdout)

    def test_removed_bridge_flags_fail_clearly(self) -> None:
        result = _run_control("--bridge-exe-path", "legacy-bridge.exe")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Legacy bridge options were removed", result.stderr)

    def test_removed_bridge_config_fields_fail_clearly(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            config_path = _write_json(
                td / "control.json",
                {
                    "schema_version": 4,
                    "default_mode": "one-shot",
                    "bridge_exe_path": "legacy-bridge.exe",
                },
            )
            result = _run_control("--config", str(config_path))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "Legacy control config field was removed: bridge_exe_path",
                result.stderr,
            )

    def test_removed_bench_runtime_policy_alias_fails_clearly(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            config_path = _write_json(
                td / "control.json",
                {
                    "schema_version": 4,
                    "default_mode": "one-shot",
                    "bench_runtime_policy_path": "runtime_policy_write_live.json",
                },
            )
            result = _run_control("--config", str(config_path))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "Legacy control config field was removed: bench_runtime_policy_path",
                result.stderr,
            )

    def test_zero_arg_staged_launch_uses_control_json_default_mode(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            staged_exe = td / "svg-mb-control.exe"
            runtime_home = td / "runtime"
            shutil.copy2(CONTROL_EXE, staged_exe)
            _write_read_loop_config(
                td,
                runtime_home=runtime_home,
                default_mode="read-loop",
                poll_ms=100,
            )

            result = _run_control(
                cwd=td,
                exe=staged_exe,
                env=_sim_direct_env(channel=1, amd_temp_c=74.0),
            )
            self.assertEqual(
                result.returncode,
                0,
                msg=f"{result.stdout}\n{result.stderr}",
            )
            self.assertIn("launched read-loop in background", result.stdout)
            match = re.search(r"pid:\s*(\d+)", result.stdout)
            self.assertIsNotNone(match, msg=result.stdout)
            child_pid = int(match.group(1))
            try:
                status = _wait_for(
                    lambda: _read_runtime_status(runtime_home),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(status, msg="control_runtime.json never appeared")
                state = _wait_for(
                    lambda: _read_runtime_current_state(runtime_home),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(state, msg="current_state.json never appeared")
                self.assertEqual(state["fans"][0]["channel"], 1)
                self.assertEqual(state["amd_sensors"][0]["temperature_c"], 74.0)
            finally:
                subprocess.run(
                    [
                        "powershell",
                        "-NoProfile",
                        "-Command",
                        f"Stop-Process -Id {child_pid} -Force -ErrorAction SilentlyContinue",
                    ],
                    capture_output=True,
                    text=True,
                )

    def test_zero_arg_staged_launch_reports_startup_failure(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            staged_exe = td / "svg-mb-control.exe"
            runtime_home = td / "runtime"
            shutil.copy2(CONTROL_EXE, staged_exe)
            _write_read_loop_config(
                td,
                runtime_home=runtime_home,
                default_mode="read-loop",
                poll_ms=100,
            )
            runtime_home.mkdir(parents=True, exist_ok=True)
            (runtime_home / "pending_writes.json").write_text(
                '{"schema_version":1,"entries":[]} trailing',
                encoding="utf-8",
            )

            result = _run_control(
                cwd=td,
                exe=staged_exe,
                env=_sim_direct_env(channel=1, amd_temp_c=74.0),
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "background process exited during startup",
                result.stderr,
            )
            self.assertIn(
                "pending writes reconciliation failed",
                result.stderr,
            )
