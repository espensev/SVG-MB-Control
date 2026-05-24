from __future__ import annotations

from tests.helpers import *


class WriteOnceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if sys.platform != "win32":
            raise unittest.SkipTest("Windows-only repo")
        _ensure_release_build()

    def test_malformed_numeric_cli_values_are_rejected(self) -> None:
        cases = [
            ("--write-channel", "-1"),
            ("--write-channel", "1x"),
            ("--write-channel", "4294967296"),
            ("--write-hold-ms", "10ms"),
            ("--write-pct", "50x"),
            ("--write-pct", "nan"),
            ("--write-pct", "101"),
        ]
        for flag, value in cases:
            with self.subTest(flag=flag, value=value):
                result = _run_control("--mode", "write-once", flag, value)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f"Invalid {flag} value", result.stderr)

    def test_write_once_happy_path(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_write_once_config(td, runtime_home=runtime_home)
            result = _run_control(
                "--mode",
                "write-once",
                "--config",
                str(config_path),
                "--write-channel",
                "0",
                "--write-pct",
                "50",
                "--write-hold-ms",
                "200",
                env=_sim_direct_env(channel=0, amd_temp_c=76.0),
            )
            self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
            self.assertEqual(_read_pending_writes(runtime_home), [])
            events = _read_runtime_events(runtime_home)
            self.assertTrue(
                any(item.get("event_type") == "write_once.write_applied" for item in events)
            )
            self.assertTrue(
                any(item.get("event_type") == "write_once.restore_applied" for item in events)
            )

    def test_write_once_uses_env_config_path_and_config_defaults(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_write_once_config(
                td,
                runtime_home=runtime_home,
                write_channel=0,
                write_target_pct=45.0,
                write_hold_ms=150,
            )
            result = _run_control(
                "--mode",
                "write-once",
                env={
                    **_sim_direct_env(channel=0, amd_temp_c=76.0),
                    "SVG_MB_CONTROL_CONFIG": str(config_path),
                },
            )
            self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
            self.assertEqual(_read_pending_writes(runtime_home), [])

    def test_write_once_baseline_stale_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_write_once_config(
                td,
                runtime_home=runtime_home,
                baseline_freshness_ceiling_ms=500,
            )
            result = _run_control(
                "--mode",
                "write-once",
                "--config",
                str(config_path),
                "--write-channel",
                "0",
                "--write-pct",
                "50",
                "--write-hold-ms",
                "200",
                env={
                    **_sim_direct_env(channel=0, amd_temp_c=76.0),
                    "SVG_MB_CONTROL_SIM_SNAPSHOT_OFFSET_MS": "5000",
                },
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("snapshot age", result.stderr)
            self.assertEqual(_read_pending_writes(runtime_home), [])

    def test_write_once_refused_on_policy_blocked(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_write_once_config(td, runtime_home=runtime_home)
            result = _run_control(
                "--mode",
                "write-once",
                "--config",
                str(config_path),
                "--write-channel",
                "0",
                "--write-pct",
                "50",
                "--write-hold-ms",
                "200",
                env={
                    **_sim_direct_env(channel=0, amd_temp_c=76.0),
                    "SVG_MB_CONTROL_SIM_EFFECTIVE_WRITE_ALLOWED": "false",
                    "SVG_MB_CONTROL_SIM_POLICY_BLOCKED": "true",
                },
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("effective_write_allowed=false", result.stderr)
            self.assertEqual(_read_pending_writes(runtime_home), [])

    def test_reconcile_runs_restore_on_startup(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            _seed_pending_writes(
                runtime_home,
                [
                    {
                        "channel": 4,
                        "baseline_duty_raw": 200,
                        "baseline_mode_raw": 5,
                        "target_pct": 60.0,
                        "requested_hold_ms": 0,
                        "started_iso": "2026-04-05T01:00:00",
                        "child_pid": 0,
                    }
                ],
            )
            config_path = _write_write_once_config(td, runtime_home=runtime_home)
            result = _run_control(
                "--config",
                str(config_path),
                env=_sim_direct_env(channel=4, amd_temp_c=76.0),
            )
            self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
            self.assertEqual(_read_pending_writes(runtime_home), [])

    def test_reconcile_failure_blocks_startup(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            _seed_pending_writes(
                runtime_home,
                [
                    {
                        "channel": 4,
                        "baseline_duty_raw": 200,
                        "baseline_mode_raw": 5,
                        "target_pct": 60.0,
                        "requested_hold_ms": 0,
                        "started_iso": "2026-04-05T01:00:00",
                        "child_pid": 0,
                    }
                ],
            )
            config_path = _write_write_once_config(td, runtime_home=runtime_home)
            result = _run_control(
                "--config",
                str(config_path),
                env={
                    **_sim_direct_env(channel=4, amd_temp_c=76.0),
                    "SVG_MB_CONTROL_SIM_RESTORE_MODE": "fail",
                },
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("reconciliation failed", result.stderr)
            self.assertEqual(len(_read_pending_writes(runtime_home)), 1)

    def test_reconcile_timeout_blocks_startup(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            _seed_pending_writes(
                runtime_home,
                [
                    {
                        "channel": 4,
                        "baseline_duty_raw": 200,
                        "baseline_mode_raw": 5,
                        "target_pct": 60.0,
                        "requested_hold_ms": 0,
                        "started_iso": "2026-04-05T01:00:00",
                        "child_pid": 0,
                    }
                ],
            )
            config_path = _write_write_once_config(
                td,
                runtime_home=runtime_home,
                restore_timeout_ms=100,
            )
            result = _run_control(
                "--config",
                str(config_path),
                env={
                    **_sim_direct_env(channel=4, amd_temp_c=76.0),
                    "SVG_MB_CONTROL_SIM_RESTORE_DELAY_MS": "500",
                },
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("reconciliation failed", result.stderr)
            self.assertEqual(len(_read_pending_writes(runtime_home)), 1)

    def test_write_once_ctrl_break_clears_pending_write(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_write_once_config(td, runtime_home=runtime_home)
            proc = _spawn_control(
                [
                    "--mode",
                    "write-once",
                    "--config",
                    str(config_path),
                    "--write-channel",
                    "0",
                    "--write-pct",
                    "50",
                    "--write-hold-ms",
                    "30000",
                ],
                env=_sim_direct_env(channel=0, amd_temp_c=76.0),
            )
            try:
                observed = _wait_for(
                    lambda: len(_read_pending_writes(runtime_home)) == 1,
                    timeout_s=5.0,
                )
                self.assertTrue(observed, msg="pending_writes.json was never created")
            finally:
                code, _, stderr = _stop_and_wait(proc)

            self.assertEqual(code, 0, msg=stderr)
            self.assertEqual(_read_pending_writes(runtime_home), [])

    def test_write_once_restore_timeout_fails_and_preserves_sidecar(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_write_once_config(
                td,
                runtime_home=runtime_home,
                restore_timeout_ms=100,
            )
            result = _run_control(
                "--mode",
                "write-once",
                "--config",
                str(config_path),
                "--write-channel",
                "0",
                "--write-pct",
                "50",
                "--write-hold-ms",
                "50",
                env={
                    **_sim_direct_env(channel=0, amd_temp_c=76.0),
                    "SVG_MB_CONTROL_SIM_RESTORE_DELAY_MS": "500",
                },
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("timed out", result.stderr)
            self.assertEqual(len(_read_pending_writes(runtime_home)), 1)
            events = _read_runtime_events(runtime_home)
            restore_failed = next(
                (
                    item
                    for item in events
                    if item.get("event_type") == "write_once.restore_failed"
                ),
                None,
            )
            self.assertIsNotNone(restore_failed)
            self.assertEqual(restore_failed["severity"], "error")
            self.assertEqual(
                restore_failed["error_code"], "WRITE_ONCE_RESTORE_FAILED"
            )
