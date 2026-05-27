from __future__ import annotations

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

    def test_show_config_text_renders_example_config(self) -> None:
        config_path = REPO_ROOT / "config" / "control.example.json"
        result = _run_control("--show-config", "--config", str(config_path))
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        out = result.stdout
        self.assertIn("svg-mb-control config:", out)
        self.assertIn("Schema version:", out)
        self.assertIn("Default mode:", out)
        self.assertIn("Loop cadence:", out)
        self.assertIn("Write timing", out)
        self.assertIn("Health/safety:", out)
        self.assertIn("Channels (", out)
        self.assertIn("ch0", out)
        self.assertIn("blend=", out)
        self.assertIn("curve  ", out)

    def test_show_config_json_renders_example_config(self) -> None:
        config_path = REPO_ROOT / "config" / "control.example.json"
        result = _run_control(
            "--show-config", "--json", "--config", str(config_path)
        )
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(data["schema_version"], 4)
        self.assertEqual(data["default_mode"], "control-loop")
        self.assertEqual(data["poll_ms"], 500)
        loop = data["control_loop"]
        self.assertIsNotNone(loop)
        self.assertEqual(loop["poll_tick_ms"], 250)
        self.assertEqual(loop["deadband_pct"], 0.25)
        self.assertTrue(loop["low_band"]["enabled"])
        channels = loop["channels"]
        self.assertGreater(len(channels), 0)
        ch0 = next(ch for ch in channels if ch["channel"] == 0)
        self.assertEqual(ch0["temp_blend"], "max_cpu_gpu_source_aware")
        self.assertEqual(ch0["source_aware_cpu_hot_guard_c"], 75.0)
        self.assertEqual(ch0["curve_shape"], "smootherstep")
        self.assertGreater(len(ch0["curve"]), 0)
        self.assertIn("thermal_pressure", ch0)
        self.assertIn("midband_pressure", ch0)
        self.assertIn("gpu_airflow", ch0)

    def test_show_config_without_config_errors(self) -> None:
        result = _run_control(
            "--show-config", "--config", "Z:/does-not-exist.json"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Control config not found", result.stderr)

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
        with StagedControlApp() as app:
            result = app.start(
                env=_sim_direct_env(channel=1, amd_temp_c=74.0),
            )
            self.assertEqual(
                result.returncode,
                0,
                msg=f"{result.stdout}\n{result.stderr}",
            )
            self.assertIn("launched read-loop in background", result.stdout)
            status = _wait_for(
                lambda: _read_runtime_status(app.runtime_home),
                timeout_s=5.0,
            )
            self.assertIsNotNone(status, msg="control_runtime.json never appeared")
            self.assertEqual(status["mode"], "read-loop")
            self.assertGreater(status["process_id"], 0)
            state = _wait_for(
                lambda: _read_runtime_current_state(app.runtime_home),
                timeout_s=5.0,
            )
            self.assertIsNotNone(state, msg="current_state.json never appeared")
            self.assertEqual(state["fans"][0]["channel"], 1)
            self.assertEqual(state["amd_sensors"][0]["temperature_c"], 74.0)

            status_result = app.status()
            self.assertEqual(status_result.returncode, 0, msg=status_result.stderr)
            self.assertIn("svg-mb-control: running", status_result.stdout)
            self.assertIn("mode: read-loop", status_result.stdout)

            stop_result = app.stop()
            self.assertEqual(
                stop_result.returncode,
                0,
                msg=f"{stop_result.stdout}\n{stop_result.stderr}",
            )
            self.assertIn("svg-mb-control: stopped", stop_result.stdout)
            stopped = _wait_for(
                lambda: (
                    s
                    if (s := _read_runtime_status(app.runtime_home))
                    and s.get("status") == "shutdown"
                    else None
                ),
                timeout_s=5.0,
            )
            self.assertIsNotNone(stopped, msg="read-loop did not publish shutdown")

    def test_supervised_launch_restarts_worker_after_crash(self) -> None:
        with StagedControlApp() as app:
            result = app.start(
                env=_sim_direct_env(channel=2, amd_temp_c=71.0),
            )
            self.assertEqual(
                result.returncode,
                0,
                msg=f"{result.stdout}\n{result.stderr}",
            )
            self.assertIn("launched read-loop in background", result.stdout)
            first_status = _wait_for(
                lambda: _read_runtime_status(app.runtime_home),
                timeout_s=5.0,
            )
            self.assertIsNotNone(
                first_status,
                msg="control_runtime.json never appeared",
            )
            first_pid = first_status["process_id"]
            self.assertGreater(first_pid, 0)

            subprocess.run(
                [
                    "powershell",
                    "-NoProfile",
                    "-Command",
                    f"Stop-Process -Id {first_pid} -Force -ErrorAction Stop",
                ],
                capture_output=True,
                text=True,
                check=True,
            )

            restarted = _wait_for(
                lambda: (
                    s
                    if (s := _read_runtime_status(app.runtime_home))
                    and s.get("status") == "running"
                    and s.get("process_id") != first_pid
                    else None
                ),
                timeout_s=8.0,
                poll_s=0.1,
            )
            self.assertIsNotNone(restarted, msg="worker was not restarted")
            self.assertGreater(restarted["process_id"], 0)
            events = _read_runtime_events(app.runtime_home)
            self.assertTrue(
                any(
                    event.get("event_type")
                    == "supervisor.worker_restart_scheduled"
                    for event in events
                ),
                msg=events,
            )

    def test_restart_stops_and_relaunches_background_worker(self) -> None:
        with StagedControlApp() as app:
            result = app.start(
                env=_sim_direct_env(channel=3, amd_temp_c=70.0),
            )
            self.assertEqual(
                result.returncode,
                0,
                msg=f"{result.stdout}\n{result.stderr}",
            )
            first_status = _wait_for(
                lambda: _read_runtime_status(app.runtime_home),
                timeout_s=5.0,
            )
            self.assertIsNotNone(first_status)
            first_pid = first_status["process_id"]

            restart_result = app.restart(
                env=_sim_direct_env(channel=4, amd_temp_c=72.0),
            )
            self.assertEqual(
                restart_result.returncode,
                0,
                msg=f"{restart_result.stdout}\n{restart_result.stderr}",
            )
            self.assertIn(
                "launched read-loop in background",
                restart_result.stdout,
            )

            restarted = _wait_for(
                lambda: (
                    s
                    if (s := _read_runtime_status(app.runtime_home))
                    and s.get("status") == "running"
                    and s.get("process_id") != first_pid
                    else None
                ),
                timeout_s=8.0,
                poll_s=0.1,
            )
            self.assertIsNotNone(restarted, msg="worker was not relaunched")
            state = _wait_for(
                lambda: (
                    s
                    if (s := _read_runtime_current_state(app.runtime_home))
                    and s["fans"][0]["channel"] == 4
                    else None
                ),
                timeout_s=5.0,
            )
            self.assertIsNotNone(state, msg="restarted worker did not refresh")
            self.assertEqual(state["amd_sensors"][0]["temperature_c"], 72.0)

            # Regression: the exiting old supervisor must not clobber the
            # successor's control_supervisor.json on an intentional restart.
            # The sidecar must track the new worker, not the pre-restart pid.
            restarted_pid = restarted["process_id"]
            supervisor_state = _wait_for(
                lambda: (
                    sv
                    if (
                        sv := _read_json(
                            app.runtime_home / "control_supervisor.json"
                        )
                    )
                    and sv.get("last_worker_pid") == restarted_pid
                    else None
                ),
                timeout_s=5.0,
                poll_s=0.1,
            )
            self.assertIsNotNone(
                supervisor_state,
                msg="control_supervisor.json was clobbered by the exiting "
                "supervisor (stale last_worker_pid after --restart)",
            )
            self.assertNotEqual(
                supervisor_state["last_worker_pid"],
                first_pid,
                msg="supervisor sidecar still shows the pre-restart worker",
            )

    def test_zero_arg_staged_launch_reports_startup_failure(self) -> None:
        with StagedControlApp() as app:
            app.runtime_home.mkdir(parents=True, exist_ok=True)
            (app.runtime_home / "pending_writes.json").write_text(
                '{"schema_version":1,"entries":[]} trailing',
                encoding="utf-8",
            )

            result = app.start(
                env=_sim_direct_env(channel=1, amd_temp_c=74.0),
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "background supervisor exited during startup",
                result.stderr,
            )
            self.assertIn(
                "pending writes reconciliation failed",
                result.stderr,
            )
