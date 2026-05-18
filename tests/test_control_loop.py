from __future__ import annotations

from tests.helpers import *


class ControlLoopTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if sys.platform != "win32":
            raise unittest.SkipTest("Windows-only repo")
        _ensure_release_build()

    def test_control_loop_ticks_and_writes(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                poll_tick_ms=50,
                write_cooldown_ms=50,
                deadband_pct=0.35,
                control_hold_ms=800,
            )
            proc = _spawn_control(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=75.0),
            )
            try:
                observed = _wait_for(
                    lambda: (_read_runtime_status(runtime_home) or {}).get(
                        "loop_tick_count", 0
                    )
                    >= 25,
                    timeout_s=8.0,
                )
                self.assertTrue(observed)
                status = _read_runtime_status(runtime_home)
                self.assertIsNotNone(status)
                self.assertEqual(status["mode"], "control-loop")
                channel_status = status["controlled_channels"][0]
                self.assertGreaterEqual(channel_status["total_writes"], 1)
                self.assertFalse(channel_status["sensor_failed"])
                self.assertEqual(channel_status["consecutive_sensor_failures"], 0)
                self.assertFalse(channel_status["circuit_breaker_open"])
                self.assertEqual(channel_status["consecutive_write_failures"], 0)
                self.assertEqual(channel_status["last_response_source"], "primary_curve")
                self.assertIn("last_write_reason", channel_status)
                self.assertTrue(status["log_csv_path"])
                self.assertTrue(status["log_manifest_path"])
                self.assertTrue(status["event_log_path"])
                self.assertTrue(Path(status["log_csv_path"]).is_file())
                self.assertTrue(Path(status["log_manifest_path"]).is_file())
                self.assertTrue((runtime_home / "logs" / "svg_mb_control_output.csv").is_file())
                timing_fields = [
                    "loop_started_wall_clock",
                    "loop_finished_wall_clock",
                    "loop_work_duration_ms",
                    "loop_intended_interval_ms",
                    "loop_achieved_interval_ms",
                    "loop_slip_ms",
                    "loop_overrun",
                    "process_cpu_delta_ms",
                    "process_cpu_pct",
                    "process_working_set_bytes",
                    "process_private_bytes",
                ]
                for field in timing_fields:
                    self.assertIn(field, status)
                self.assertEqual(status["loop_intended_interval_ms"], 50)
                self.assertGreaterEqual(status["loop_work_duration_ms"], 0)
                self.assertGreaterEqual(status["loop_achieved_interval_ms"], 0)
                self.assertIsInstance(status["loop_overrun"], bool)
                self.assertGreaterEqual(status["process_cpu_delta_ms"], 0)
                self.assertGreaterEqual(status["process_cpu_pct"], 0)
                self.assertGreater(status["process_working_set_bytes"], 0)
                self.assertGreater(status["process_private_bytes"], 0)

                rows = _wait_for(
                    lambda: _read_runtime_csv_rows(Path(status["log_csv_path"])),
                    timeout_s=5.0,
                )
                self.assertTrue(rows, msg="control-loop CSV rows missing")
                latest_row = rows[-1]
                for field in timing_fields:
                    self.assertIn(field, latest_row)
                self.assertEqual(latest_row["loop_intended_interval_ms"], "50")
                self.assertIn("channel0_thermal_pressure_boost_pct", latest_row)
                self.assertIn("channel0_response_source", latest_row)
                self.assertIn("channel0_write_reason", latest_row)
                self.assertIn("channel0_feedforward_pct", latest_row)
                self.assertIn("channel0_correction_pct", latest_row)
                self.assertEqual(latest_row["channel0_response_source"], "primary_curve")
                self.assertNotEqual(latest_row["channel0_write_reason"], "")
                self.assertNotEqual(latest_row["channel0_feedforward_pct"], "")
                self.assertNotEqual(latest_row["channel0_correction_pct"], "")
                self.assertNotEqual(latest_row["loop_work_duration_ms"], "")
                self.assertNotEqual(latest_row["loop_achieved_interval_ms"], "")
                self.assertIn(latest_row["loop_overrun"], ("true", "false"))
                self.assertNotEqual(latest_row["process_cpu_delta_ms"], "")
                self.assertNotEqual(latest_row["process_cpu_pct"], "")
                self.assertNotEqual(latest_row["process_working_set_bytes"], "")
                self.assertNotEqual(latest_row["process_private_bytes"], "")
                event_observed = _wait_for(
                    lambda: any(
                        item.get("event_type") == "control_loop.write_applied"
                        for item in _read_runtime_events(runtime_home)
                    ),
                    timeout_s=5.0,
                )
                self.assertTrue(event_observed)
            finally:
                _stop_and_wait(proc)

    def test_control_loop_limits_first_write_from_observed_baseline(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                poll_tick_ms=250,
                write_cooldown_ms=250,
                deadband_pct=0.25,
                control_hold_ms=0,
                curve=[(30.0, 20.0), (60.0, 80.0), (95.0, 100.0)],
                min_duty_pct=20.0,
                extra_channel_fields={
                    "rise_rate_pct_per_min": 90.0,
                    "fall_rate_pct_per_min": 45.0,
                    "max_setpoint_step_pct": 0.5,
                },
            )
            proc = _spawn_control(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=75.0, duty_raw=51),
            )
            try:
                write_event = _wait_for(
                    lambda: next(
                        (
                            item
                            for item in _read_runtime_events(runtime_home)
                            if item.get("event_type") == "control_loop.write_applied"
                        ),
                        None,
                    ),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(write_event)
                self.assertEqual(write_event["channel"], 0)
                self.assertGreater(write_event["setpoint_pct"], 20.0)
                self.assertLessEqual(write_event["setpoint_pct"], 20.55)
            finally:
                _stop_and_wait(proc)

    def test_control_loop_current_state_prefers_direct_fan_telemetry(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
            )
            proc = _spawn_control(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(
                    channel=0,
                    amd_temp_c=75.0,
                    duty_raw=80,
                    mode_raw=3,
                    read_channel=0,
                    read_duty_raw=222,
                    read_mode_raw=7,
                ),
            )
            try:
                state = _wait_for(
                    lambda: _read_runtime_current_state(runtime_home),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(state, msg="current_state.json never appeared")
                fan0 = next(fan for fan in state["fans"] if fan["channel"] == 0)
                self.assertEqual(fan0["duty_raw"], 222)
                self.assertEqual(fan0["mode_raw"], 7)
            finally:
                _stop_and_wait(proc)

    def test_control_loop_cpu_override_can_drive_gpu_blend_channel(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                temp_blend="gpu_only",
                min_duty_pct=20.0,
                curve=[(50.0, 20.0), (90.0, 100.0)],
                cpu_override_curve=[(75.0, 20.0), (85.0, 70.0)],
                control_hold_ms=0,
            )
            proc = _spawn_control(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=86.0),
            )
            try:
                status = _wait_for(
                    lambda: (
                        status
                        if (status := _read_runtime_status(runtime_home))
                        and status["controlled_channels"][0]["total_writes"] >= 1
                        else None
                    ),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(status)
                channel = status["controlled_channels"][0]
                self.assertGreaterEqual(channel["last_setpoint_pct"], 69.0)
                self.assertGreaterEqual(channel["total_writes"], 1)
                self.assertEqual(channel["last_response_source"], "cpu_override")
            finally:
                _stop_and_wait(proc)

    def test_control_loop_thermal_pressure_boost_accumulates_under_sustained_heat(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                poll_tick_ms=50,
                write_cooldown_ms=50,
                deadband_pct=0.1,
                control_hold_ms=0,
                min_duty_pct=20.0,
                curve=[(50.0, 20.0), (100.0, 20.0)],
                extra_channel_fields={
                    "thermal_pressure_start_c": 80.0,
                    "thermal_pressure_full_c": 90.0,
                    "thermal_pressure_rise_pct_per_sec": 25.0,
                    "thermal_pressure_fall_pct_per_sec": 2.0,
                    "thermal_pressure_max_boost_pct": 30.0,
                },
            )
            proc = _spawn_control(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=86.0),
            )
            try:
                status = _wait_for(
                    lambda: (
                        status
                        if (status := _read_runtime_status(runtime_home))
                        and status["controlled_channels"][0][
                            "last_thermal_pressure_boost_pct"
                        ]
                        >= 5.0
                        else None
                    ),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(status)
                channel = status["controlled_channels"][0]
                self.assertGreaterEqual(
                    channel["last_thermal_pressure_boost_pct"], 5.0
                )
                self.assertIn("thermal_pressure", channel["last_response_source"])
                self.assertGreaterEqual(channel["last_setpoint_pct"], 25.0)
                self.assertGreaterEqual(channel["total_writes"], 2)
            finally:
                _stop_and_wait(proc)

    def test_control_loop_cpu_low_soak_accumulates_under_sustained_mid_cpu(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                poll_tick_ms=50,
                write_cooldown_ms=50,
                deadband_pct=0.1,
                control_hold_ms=0,
                min_duty_pct=20.0,
                curve=[(50.0, 20.0), (100.0, 20.0)],
                extra_channel_fields={
                    "cpu_low_soak_start_c": 62.0,
                    "cpu_low_soak_full_c": 66.0,
                    "cpu_low_soak_release_c": 60.0,
                    "cpu_low_soak_rise_pct_per_min": 120.0,
                    "cpu_low_soak_fall_pct_per_min": 60.0,
                    "cpu_low_soak_max_boost_pct": 5.0,
                },
            )
            proc = _spawn_control(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=66.0),
            )
            try:
                status = _wait_for(
                    lambda: (
                        status
                        if (status := _read_runtime_status(runtime_home))
                        and status["controlled_channels"][0][
                            "last_cpu_low_soak_boost_pct"
                        ]
                        >= 2.0
                        else None
                    ),
                    timeout_s=6.0,
                )
                self.assertIsNotNone(status)
                channel = status["controlled_channels"][0]
                self.assertIn("cpu_low_soak", channel["last_response_source"])
                self.assertGreaterEqual(channel["last_setpoint_pct"], 22.0)
                self.assertGreaterEqual(channel["total_writes"], 2)

                rows = _read_runtime_csv_rows(Path(status["log_csv_path"]))
                self.assertTrue(rows)
                latest_row = rows[-1]
                self.assertIn("channel0_cpu_low_soak_boost_pct", latest_row)
                self.assertIn("cpu_low_soak", latest_row["channel0_response_source"])
            finally:
                _stop_and_wait(proc)

    def test_control_loop_cpu_low_soak_stays_zero_below_release(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                poll_tick_ms=50,
                write_cooldown_ms=50,
                deadband_pct=0.1,
                control_hold_ms=0,
                min_duty_pct=20.0,
                curve=[(50.0, 20.0), (100.0, 20.0)],
                extra_channel_fields={
                    "cpu_low_soak_start_c": 62.0,
                    "cpu_low_soak_full_c": 66.0,
                    "cpu_low_soak_release_c": 60.0,
                    "cpu_low_soak_rise_pct_per_min": 120.0,
                    "cpu_low_soak_fall_pct_per_min": 60.0,
                    "cpu_low_soak_max_boost_pct": 5.0,
                },
            )
            proc = _spawn_control(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=59.0),
            )
            try:
                observed = _wait_for(
                    lambda: (
                        status
                        if (status := _read_runtime_status(runtime_home))
                        and status.get("loop_tick_count", 0) >= 25
                        else None
                    ),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(observed)
                channel = observed["controlled_channels"][0]
                self.assertEqual(channel["last_cpu_low_soak_boost_pct"], 0.0)
                self.assertEqual(channel["last_response_source"], "primary_curve")
                self.assertEqual(channel["last_setpoint_pct"], 20.0)
            finally:
                _stop_and_wait(proc)

    def test_control_loop_low_band_stage_accumulates_smooth_trim_and_evidence(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                poll_tick_ms=50,
                write_cooldown_ms=50,
                deadband_pct=0.1,
                control_hold_ms=0,
                min_duty_pct=20.0,
                curve=[(50.0, 20.0), (100.0, 20.0)],
                extra_loop_fields={
                    "low_band": {
                        "enabled": True,
                        "cpu_start_c": 62.0,
                        "cpu_full_c": 66.0,
                        "cpu_release_c": 60.0,
                        "gpu_start_c": 68.0,
                        "gpu_full_c": 76.0,
                        "gpu_release_c": 64.0,
                        "rise_per_min": 180.0,
                        "fall_per_min": 60.0,
                        "stage_rise_pct_per_min": 120.0,
                        "stage_fall_pct_per_min": 120.0,
                        "stage_spacing_ms": 100,
                        "evidence_write_interval_ms": 200,
                    },
                },
                extra_channel_fields={
                    "low_band_stage": 1,
                    "low_band_debt_threshold": 0.05,
                    "low_band_hold_ms": 100,
                    "low_band_max_boost_pct": 3.0,
                },
            )
            proc = _spawn_control(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=66.0),
            )
            try:
                status = _wait_for(
                    lambda: (
                        status
                        if (status := _read_runtime_status(runtime_home))
                        and status["controlled_channels"][0][
                            "last_low_band_stage_boost_pct"
                        ]
                        >= 0.5
                        else None
                    ),
                    timeout_s=6.0,
                )
                self.assertIsNotNone(status)
                channel = status["controlled_channels"][0]
                self.assertIn("low_band_stage", channel["last_response_source"])
                self.assertTrue(channel["low_band_stage_active"])
                self.assertGreaterEqual(channel["last_setpoint_pct"], 20.5)
                self.assertGreaterEqual(channel["low_band_activation_count"], 1)

                rows = _read_runtime_csv_rows(Path(status["log_csv_path"]))
                self.assertTrue(rows)
                latest_row = rows[-1]
                self.assertIn("channel0_low_band_stage_boost_pct", latest_row)
                self.assertIn("channel0_low_band_debt", latest_row)
                self.assertIn("channel0_low_band_stage_active", latest_row)

                evidence = _wait_for(
                    lambda: _read_json(runtime_home / "low_band_evidence.json"),
                    timeout_s=3.0,
                )
                self.assertIsNotNone(evidence)
                self.assertTrue(evidence["enabled"])
                self.assertGreater(evidence["debt"], 0.0)
                self.assertGreaterEqual(
                    evidence["channels"][0]["activation_count"], 1
                )
            finally:
                _stop_and_wait(proc)

    def test_control_loop_uses_local_runtime_policy_gate(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            policy_path = td / "runtime_policy.json"
            _write_json(
                policy_path,
                {
                    "schema_version": 1,
                    "profile_name": "blocked",
                    "board": "test",
                    "control": {
                        "writes_enabled": True,
                        "restore_on_exit": True,
                        "blocked_channels": [0],
                    },
                },
            )
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                runtime_policy_path=policy_path,
            )
            proc = _spawn_control(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=82.0),
            )
            try:
                state = _wait_for(
                    lambda: _read_runtime_current_state(runtime_home),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(state)
                fan0 = next(fan for fan in state["fans"] if fan["channel"] == 0)
                self.assertTrue(fan0["policy_blocked"])
                self.assertFalse(fan0["effective_write_allowed"])

                status = _wait_for(
                    lambda: _read_runtime_status(runtime_home),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(status)
                self.assertEqual(status["controlled_channels"][0]["total_writes"], 0)
            finally:
                _stop_and_wait(proc)

    def test_control_loop_empty_channels_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_json(
                td / "control.json",
                {
                    "schema_version": 4,
                    "runtime_home_path": runtime_home.as_posix(),
                    "control_loop": {
                        "poll_tick_ms": 200,
                        "channels": [],
                    },
                },
            )
            result = _run_control(
                "--mode",
                "control-loop",
                "--config",
                str(config_path),
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("empty channels array", result.stderr)

    def test_control_loop_empty_channel_curve_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_json(
                td / "control.json",
                {
                    "schema_version": 4,
                    "runtime_home_path": runtime_home.as_posix(),
                    "control_loop": {
                        "poll_tick_ms": 200,
                        "channels": [
                            {
                                "channel": 0,
                                "temp_blend": "cpu_only",
                                "min_duty_pct": 30.0,
                                "curve": [],
                            }
                        ],
                    },
                },
            )
            result = _run_control(
                "--mode",
                "control-loop",
                "--config",
                str(config_path),
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("curve must not be empty", result.stderr)

    def test_control_loop_channel_hold_override_flows_to_pending_write(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                poll_tick_ms=100,
                write_cooldown_ms=100,
                deadband_pct=2.0,
                control_hold_ms=800,
                channel_control_hold_ms=2222,
            )
            proc = _spawn_control(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=75.0),
            )
            try:
                entry = _wait_for(
                    lambda: next(
                        (
                            item
                            for item in _read_pending_writes(runtime_home)
                            if item.get("channel") == 0
                        ),
                        None,
                    ),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(entry, msg="pending write entry was not created")
                self.assertEqual(entry["requested_hold_ms"], 2222)
            finally:
                _stop_and_wait(proc)

    def test_control_loop_zero_hold_flows_to_pending_write(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                poll_tick_ms=100,
                write_cooldown_ms=100,
                deadband_pct=2.0,
                control_hold_ms=0,
            )
            proc = _spawn_control(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=75.0),
            )
            try:
                entry = _wait_for(
                    lambda: next(
                        (
                            item
                            for item in _read_pending_writes(runtime_home)
                            if item.get("channel") == 0
                        ),
                        None,
                    ),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(entry, msg="pending write entry was not created")
                self.assertEqual(entry["requested_hold_ms"], 0)
            finally:
                _stop_and_wait(proc)

    def test_control_loop_policy_refusal_flushes_pending_write(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                poll_tick_ms=50,
                write_cooldown_ms=5000,
                deadband_pct=0.1,
                control_hold_ms=800,
            )
            proc = _spawn_control(
                ["--mode", "control-loop", "--config", str(config_path)],
                env={
                    **_sim_direct_env(channel=0, amd_temp_c=75.0),
                    "SVG_MB_CONTROL_SIM_WRITE_MODE": "policy_refused",
                },
            )
            try:
                refused_and_flushed = _wait_for(
                    lambda: (
                        not _read_pending_writes(runtime_home)
                        and any(
                            item.get("event_type") == "control_loop.write_failed"
                            and item.get("success") is False
                            for item in _read_runtime_events(runtime_home)
                        )
                    ),
                    timeout_s=5.0,
                )
                self.assertTrue(
                    refused_and_flushed,
                    msg="policy-refused pending write entry was not flushed",
                )
            finally:
                _stop_and_wait(proc)

    def test_control_loop_restore_timeout_aborts_and_preserves_sidecar(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                restore_timeout_ms=100,
                poll_tick_ms=100,
                write_cooldown_ms=100,
                deadband_pct=2.0,
                control_hold_ms=150,
            )
            proc = _spawn_control(
                ["--mode", "control-loop", "--config", str(config_path)],
                env={
                    **_sim_direct_env(channel=0, amd_temp_c=75.0),
                    "SVG_MB_CONTROL_SIM_RESTORE_DELAY_MS": "500",
                },
            )
            code = None
            stdout = ""
            stderr = ""
            try:
                exited = _wait_for(
                    lambda: proc.poll() is not None,
                    timeout_s=5.0,
                )
                self.assertTrue(exited, msg="control-loop never exited after restore timeout")
                stdout, stderr = proc.communicate(timeout=1.0)
                code = proc.returncode
            finally:
                if proc.poll() is None:
                    code, stdout, stderr = _stop_and_wait(proc)

            self.assertNotEqual(code, 0, msg=f"{stdout}\n{stderr}")
            self.assertEqual(len(_read_pending_writes(runtime_home)), 1)
            status = _read_runtime_status(runtime_home)
            self.assertIsNotNone(status)
            self.assertEqual(status["status_detail"], "restore failed")
            events = _read_runtime_events(runtime_home)
            self.assertTrue(
                any(item.get("event_type") == "control_loop.abort" for item in events)
            )
