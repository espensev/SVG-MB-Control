from __future__ import annotations

from tests.helpers import *


class ControlLoopTests(WindowsExeTestCase):
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
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=75.0),
            ):
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
                self.assertIn("last_low_band_effective_boost_pct", channel_status)
                self.assertIn("last_write_reason", channel_status)
                self.assertTrue(status["log_csv_path"])
                self.assertTrue(status["log_manifest_path"])
                self.assertTrue(status["event_log_path"])
                self.assertTrue(Path(status["log_csv_path"]).is_file())
                self.assertTrue(Path(status["log_manifest_path"]).is_file())
                self.assertTrue((runtime_home / "logs" / "svg_mb_control_output.csv").is_file())
                prologue = _read_runtime_csv_prologue(Path(status["log_csv_path"]))
                self.assertEqual(prologue["producer"], "svg-mb-control")
                self.assertIn("build_version", prologue)
                self.assertIn("git_hash", prologue)
                self.assertEqual(
                    Path(prologue["config_path"]).resolve(),
                    config_path.resolve(),
                )
                self.assertRegex(prologue["config_sha256"], r"^[0-9a-f]{64}$")
                self.assertEqual(prologue["control_poll_tick_ms"], "50")
                self.assertEqual(prologue["control_write_cooldown_ms"], "50")
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
                    "cadence_transient",
                ]
                for field in timing_fields:
                    self.assertIn(field, status)
                self.assertEqual(status["loop_intended_interval_ms"], 50)
                self.assertEqual(float(status["cadence_transient"]), 0.0)
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
                self.assertEqual(float(latest_row["cadence_transient"]), 0.0)
                self.assertIn("channel0_thermal_pressure_boost_pct", latest_row)
                self.assertIn("channel0_midband_pressure_boost_pct", latest_row)
                self.assertIn("channel0_gpu_airflow_boost_pct", latest_row)
                self.assertIn("channel0_low_band_effective_boost_pct", latest_row)
                self.assertIn("channel0_primary_temp_source", latest_row)
                self.assertIn("channel0_response_source", latest_row)
                self.assertIn("channel0_write_reason", latest_row)
                self.assertIn("channel0_feedforward_pct", latest_row)
                self.assertIn("channel0_correction_pct", latest_row)
                self.assertEqual(latest_row["channel0_primary_temp_source"], "cpu")
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
                self.assertEqual(write_event["severity"], "info")
                self.assertEqual(write_event["error_code"], "none")

    def test_control_loop_pid_shadow_computes_but_does_not_write(self) -> None:
        # FEAT-0003 (REQ-PROFILE-07/08): a PID channel with no pid.allow_live runs
        # shadow/dry-run -- it computes + logs a setpoint and is attributed to the
        # pid law, but never actuates the fan (no writes) and records the shadow
        # law at startup.
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
                min_duty_pct=40.0,
                extra_channel_fields={
                    "controller": "pid",
                    "pid": {
                        "target_c": 60.0,
                        "kp": 2.0,
                        "feedforward": "fixed",
                        "fixed_feedforward_pct": 30.0,
                    },
                },
            )
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=75.0),
            ):
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
                channel_status = status["controlled_channels"][0]
                # Attribution + kind-aware PID evidence (REQ-PROFILE-08).
                self.assertEqual(channel_status["controller_kind"], "pid")
                self.assertEqual(channel_status["last_response_source"], "pid")
                self.assertIsNotNone(channel_status["pid_error_c"])
                # REQ-PROFILE-08: curve-only telemetry is null for a PID channel,
                # not published as a meaningful 0.0.
                self.assertIsNone(channel_status["last_raw_demand_pct"])
                self.assertIsNone(channel_status["last_smoothed_demand_pct"])
                self.assertIsNone(channel_status["last_thermal_pressure_boost_pct"])
                # Shadow/dry-run (REQ-PROFILE-07): the PID computed a setpoint, but
                # the channel never actuated -- no writes despite 25+ ticks at 75C.
                self.assertEqual(channel_status["total_writes"], 0)
                # The worker recorded the shadow law at startup.
                shadow_event = next(
                    (
                        item
                        for item in _read_runtime_events(runtime_home)
                        if item.get("event_type") == "control_loop.pid_shadow"
                    ),
                    None,
                )
                self.assertIsNotNone(shadow_event)
                self.assertIn("allow_live", shadow_event.get("detail", ""))
                # CSV carries the additive kind-aware columns; the curve-only
                # feedforward column blanks for a PID channel.
                rows = _wait_for(
                    lambda: _read_runtime_csv_rows(Path(status["log_csv_path"])),
                    timeout_s=5.0,
                )
                self.assertTrue(rows, msg="pid-shadow CSV rows missing")
                latest_row = rows[-1]
                self.assertEqual(latest_row["channel0_controller_kind"], "pid")
                self.assertNotEqual(latest_row["channel0_pid_error_c"], "")
                self.assertEqual(latest_row["channel0_feedforward_pct"], "")

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
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=75.0, duty_raw=51),
            ):
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

    def test_control_loop_current_state_prefers_direct_fan_telemetry(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
            )
            with RuntimeProbe(
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
            ):
                state = _wait_for(
                    lambda: _read_runtime_current_state(runtime_home),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(state, msg="current_state.json never appeared")
                fan0 = next(fan for fan in state["fans"] if fan["channel"] == 0)
                self.assertEqual(fan0["duty_raw"], 222)
                self.assertEqual(fan0["mode_raw"], 7)

    def test_control_loop_retries_current_state_publish_after_failure(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            blocked_snapshot = runtime_home / "current_state.json"
            blocked_snapshot.mkdir(parents=True)
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                poll_tick_ms=50,
                write_cooldown_ms=50,
                deadband_pct=0.35,
                control_hold_ms=800,
            )
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=75.0),
            ):
                failed = _wait_for(
                    lambda: next(
                        (
                            item
                            for item in _read_runtime_events(runtime_home)
                            if item.get("event_type")
                            == "runtime_logging.snapshot_publish_failed"
                        ),
                        None,
                    ),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(failed)
                self.assertIn("current_state.json", failed["detail"])

                shutil.rmtree(blocked_snapshot)
                state = _wait_for(
                    lambda: _read_runtime_current_state(runtime_home),
                    timeout_s=0.85,
                    poll_s=0.02,
                )
                self.assertIsNotNone(
                    state,
                    msg="current_state.json was not retried promptly after failure",
                )
                recovered = _wait_for(
                    lambda: next(
                        (
                            item
                            for item in _read_runtime_events(runtime_home)
                            if item.get("event_type")
                            == "runtime_logging.snapshot_publish_recovered"
                        ),
                        None,
                    ),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(recovered)

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
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env={
                    **_sim_direct_env(channel=0, amd_temp_c=86.0),
                    "SVG_MB_CONTROL_SIM_GPU_MODE": "enabled",
                    "SVG_MB_CONTROL_SIM_GPU_CORE_C": "50.0",
                    "SVG_MB_CONTROL_SIM_GPU_MEMJN_C": "50.0",
                    "SVG_MB_CONTROL_SIM_GPU_HOTSPOT_C": "50.0",
                },
            ):
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

    def test_control_loop_source_aware_blend_uses_gpu_below_cpu_guard(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                temp_blend="max_cpu_gpu_source_aware",
                min_duty_pct=20.0,
                curve=[(50.0, 20.0), (90.0, 100.0)],
                control_hold_ms=0,
                extra_channel_fields={
                    "source_aware_cpu_hot_guard_c": 75.0,
                },
            )
            env = _sim_direct_env(channel=0, amd_temp_c=70.0)
            env.update(
                {
                    "SVG_MB_CONTROL_SIM_GPU_MODE": "enabled",
                    "SVG_MB_CONTROL_SIM_GPU_CORE_C": "50.0",
                    "SVG_MB_CONTROL_SIM_GPU_MEMJN_C": "50.0",
                    "SVG_MB_CONTROL_SIM_GPU_HOTSPOT_C": "50.0",
                }
            )
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=env,
            ):
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
                self.assertEqual(channel["last_primary_temp_source"], "gpu")
                self.assertAlmostEqual(channel["last_raw_demand_pct"], 20.0, places=2)

                rows = _wait_for(
                    lambda: _read_runtime_csv_rows(Path(status["log_csv_path"])),
                    timeout_s=5.0,
                )
                self.assertTrue(rows)
                self.assertEqual(rows[-1]["channel0_primary_temp_source"], "gpu")

    def test_control_loop_source_aware_guard_preserves_cpu_hot_max_blend(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                temp_blend="max_cpu_gpu_source_aware",
                min_duty_pct=20.0,
                curve=[(50.0, 20.0), (90.0, 100.0)],
                control_hold_ms=0,
                extra_channel_fields={
                    "source_aware_cpu_hot_guard_c": 75.0,
                },
            )
            env = _sim_direct_env(channel=0, amd_temp_c=80.0)
            env.update(
                {
                    "SVG_MB_CONTROL_SIM_GPU_MODE": "enabled",
                    "SVG_MB_CONTROL_SIM_GPU_CORE_C": "50.0",
                    "SVG_MB_CONTROL_SIM_GPU_MEMJN_C": "50.0",
                    "SVG_MB_CONTROL_SIM_GPU_HOTSPOT_C": "50.0",
                }
            )
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=env,
            ):
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
                self.assertEqual(channel["last_primary_temp_source"], "cpu_guard")
                self.assertAlmostEqual(channel["last_raw_demand_pct"], 80.0, places=2)

    def test_control_loop_source_aware_missing_gpu_falls_back_to_cpu(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                temp_blend="max_cpu_gpu_source_aware",
                min_duty_pct=20.0,
                curve=[(50.0, 20.0), (90.0, 100.0)],
                control_hold_ms=0,
                extra_channel_fields={
                    "source_aware_cpu_hot_guard_c": 75.0,
                },
            )
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env={
                    **_sim_direct_env(channel=0, amd_temp_c=70.0),
                    "SVG_MB_CONTROL_SIM_GPU_MODE": "unavailable",
                },
            ):
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
                self.assertEqual(channel["last_primary_temp_source"], "cpu_fallback")
                self.assertFalse(channel["sensor_failed"])
                self.assertEqual(channel["consecutive_sensor_failures"], 0)
                self.assertAlmostEqual(channel["last_raw_demand_pct"], 60.0, places=2)

                rows = _wait_for(
                    lambda: _read_runtime_csv_rows(Path(status["log_csv_path"])),
                    timeout_s=5.0,
                )
                self.assertTrue(rows)
                self.assertEqual(
                    rows[-1]["channel0_primary_temp_source"], "cpu_fallback"
                )

    def test_control_loop_logs_gpu_board_power(self) -> None:
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
                control_hold_ms=0,
            )
            env = _sim_direct_env(channel=0, amd_temp_c=70.0)
            env.update(
                {
                    "SVG_MB_CONTROL_SIM_GPU_MODE": "enabled",
                    "SVG_MB_CONTROL_SIM_GPU_CORE_C": "60.0",
                    "SVG_MB_CONTROL_SIM_GPU_MEMJN_C": "70.0",
                    "SVG_MB_CONTROL_SIM_GPU_HOTSPOT_C": "70.0",
                    "SVG_MB_CONTROL_SIM_GPU_NVML_POWER_MW": "275000",
                    "SVG_MB_CONTROL_SIM_GPU_POWER_SOURCE": "nvml",
                    "SVG_MB_CONTROL_SIM_GPU_UTIL_GPU_PCT": "68",
                    "SVG_MB_CONTROL_SIM_GPU_UTIL_MEM_PCT": "22",
                    "SVG_MB_CONTROL_SIM_GPU_PSTATE": "0",
                    "SVG_MB_CONTROL_SIM_GPU_CLOCK_GRAPHICS_MHZ": "2490",
                    "SVG_MB_CONTROL_SIM_GPU_CLOCK_MEMORY_MHZ": "10490",
                    "SVG_MB_CONTROL_SIM_GPU_VRAM_USED_MB": "8192",
                    "SVG_MB_CONTROL_SIM_GPU_VRAM_TOTAL_MB": "16384",
                }
            )
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=env,
            ):
                status = _wait_for(
                    lambda: (
                        status
                        if (status := _read_runtime_status(runtime_home))
                        and status.get("loop_tick_count", 0) >= 5
                        else None
                    ),
                    timeout_s=6.0,
                )
                self.assertIsNotNone(status)
                rows = _wait_for(
                    lambda: _read_runtime_csv_rows(Path(status["log_csv_path"])),
                    timeout_s=5.0,
                )
                self.assertTrue(rows, msg="control-loop CSV rows missing")
                latest = rows[-1]
                for col in (
                    "gpu_power_sample_id",
                    "gpu_power_time_ms",
                    "gpu_power_mw",
                    "gpu_power_source",
                    "gpu_power_acquisition",
                    "gpu_context_sample_id",
                    "gpu_context_time_ms",
                    "gpu_context_sample_age_ms",
                    "gpu_context_acquisition",
                    "gpu_util_gpu_pct",
                    "gpu_util_mem_pct",
                    "gpu_pstate",
                    "gpu_clock_graphics_mhz",
                    "gpu_clock_memory_mhz",
                    "gpu_vram_used_mb",
                    "gpu_vram_total_mb",
                ):
                    self.assertIn(col, latest)
                # A fresh nonzero NVML read: source/acquisition both "nvml", the
                # value matches the simulated board power, and the read identity
                # (sample id + timestamp) is populated.
                self.assertEqual(latest["gpu_power_source"], "nvml")
                self.assertEqual(latest["gpu_power_acquisition"], "nvml")
                self.assertNotEqual(latest["gpu_power_mw"], "")
                self.assertAlmostEqual(
                    float(latest["gpu_power_mw"]), 275000.0, places=0
                )
                self.assertNotEqual(latest["gpu_power_sample_id"], "")
                self.assertGreaterEqual(int(latest["gpu_power_sample_id"]), 1)
                self.assertNotEqual(latest["gpu_power_time_ms"], "")
                # FEAT-0021 context rides beside power as logging-only cached
                # context. The simulation path produces a fresh nvml context
                # sample and no false-zero blanks.
                self.assertEqual(latest["gpu_context_acquisition"], "nvml")
                self.assertNotEqual(latest["gpu_context_sample_id"], "")
                self.assertNotEqual(latest["gpu_context_time_ms"], "")
                self.assertNotEqual(latest["gpu_context_sample_age_ms"], "")
                self.assertEqual(latest["gpu_util_gpu_pct"], "68")
                self.assertEqual(latest["gpu_util_mem_pct"], "22")
                self.assertEqual(latest["gpu_pstate"], "0")
                self.assertEqual(latest["gpu_clock_graphics_mhz"], "2490")
                self.assertEqual(latest["gpu_clock_memory_mhz"], "10490")
                self.assertEqual(latest["gpu_vram_used_mb"], "8192")
                self.assertEqual(latest["gpu_vram_total_mb"], "16384")
                # Power must not become a control input: response source stays
                # temperature-derived.
                self.assertEqual(
                    latest["channel0_response_source"], "primary_curve"
                )

    def test_control_loop_gpu_power_unavailable_emits_no_false_zero(self) -> None:
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
                control_hold_ms=0,
            )
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env={
                    **_sim_direct_env(channel=0, amd_temp_c=70.0),
                    "SVG_MB_CONTROL_SIM_GPU_MODE": "unavailable",
                },
            ):
                status = _wait_for(
                    lambda: (
                        status
                        if (status := _read_runtime_status(runtime_home))
                        and status.get("loop_tick_count", 0) >= 5
                        else None
                    ),
                    timeout_s=6.0,
                )
                self.assertIsNotNone(status)
                rows = _wait_for(
                    lambda: _read_runtime_csv_rows(Path(status["log_csv_path"])),
                    timeout_s=5.0,
                )
                self.assertTrue(rows)
                latest = rows[-1]
                # No false zero: an unavailable GPU power read leaves the numeric
                # fields blank and records why in the acquisition marker.
                self.assertEqual(latest["gpu_power_mw"], "")
                self.assertEqual(latest["gpu_power_sample_id"], "")
                self.assertEqual(latest["gpu_power_time_ms"], "")
                self.assertEqual(latest["gpu_power_acquisition"], "unavailable")
                self.assertEqual(latest["gpu_context_sample_id"], "")
                self.assertEqual(latest["gpu_context_time_ms"], "")
                self.assertEqual(latest["gpu_context_sample_age_ms"], "")
                self.assertEqual(latest["gpu_context_acquisition"], "unavailable")
                self.assertEqual(latest["gpu_util_gpu_pct"], "")
                self.assertEqual(latest["gpu_vram_total_mb"], "")

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
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=86.0),
            ):
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

    def test_control_loop_midband_pressure_accumulates_in_mid_band(self) -> None:
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
                    "midband_pressure_start_c": 64.0,
                    "midband_pressure_full_c": 84.0,
                    "midband_pressure_rise_pct_per_sec": 25.0,
                    "midband_pressure_fall_pct_per_sec": 2.0,
                    "midband_pressure_max_boost_pct": 12.0,
                },
            )
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=74.0),
            ):
                status = _wait_for(
                    lambda: (
                        status
                        if (status := _read_runtime_status(runtime_home))
                        and status["controlled_channels"][0][
                            "last_midband_pressure_boost_pct"
                        ]
                        >= 4.0
                        else None
                    ),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(status)
                channel = status["controlled_channels"][0]
                self.assertGreaterEqual(
                    channel["last_midband_pressure_boost_pct"], 4.0
                )
                self.assertIn(
                    "midband_pressure", channel["last_response_source"]
                )
                self.assertGreaterEqual(channel["last_setpoint_pct"], 24.0)
                self.assertGreaterEqual(channel["total_writes"], 2)
                rows = _wait_for(
                    lambda: _read_runtime_csv_rows(Path(status["log_csv_path"])),
                    timeout_s=5.0,
                )
                self.assertTrue(rows)
                latest_row = rows[-1]
                self.assertIn("channel0_midband_pressure_boost_pct", latest_row)
                self.assertGreaterEqual(
                    float(latest_row["channel0_midband_pressure_boost_pct"]),
                    4.0,
                )

    def test_control_loop_gpu_airflow_starts_on_warm_gpu(self) -> None:
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
                    "gpu_airflow_start_c": 62.0,
                    "gpu_airflow_full_c": 80.0,
                    "gpu_airflow_rise_pct_per_sec": 25.0,
                    "gpu_airflow_fall_pct_per_sec": 2.0,
                    "gpu_airflow_max_boost_pct": 12.0,
                },
            )
            # CPU held cool so only the GPU envelope drives a response.
            env = _sim_direct_env(channel=0, amd_temp_c=45.0)
            env.update(
                {
                    "SVG_MB_CONTROL_SIM_GPU_MODE": "enabled",
                    "SVG_MB_CONTROL_SIM_GPU_CORE_C": "60.0",
                    "SVG_MB_CONTROL_SIM_GPU_MEMJN_C": "72.0",
                    "SVG_MB_CONTROL_SIM_GPU_HOTSPOT_C": "70.0",
                }
            )
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=env,
            ):
                status = _wait_for(
                    lambda: (
                        status
                        if (status := _read_runtime_status(runtime_home))
                        and status["controlled_channels"][0][
                            "last_gpu_airflow_boost_pct"
                        ]
                        >= 4.0
                        else None
                    ),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(status)
                channel = status["controlled_channels"][0]
                self.assertGreaterEqual(
                    channel["last_gpu_airflow_boost_pct"], 4.0
                )
                self.assertIn("gpu_airflow", channel["last_response_source"])
                self.assertGreaterEqual(channel["last_setpoint_pct"], 24.0)
                self.assertGreaterEqual(channel["total_writes"], 2)
                rows = _wait_for(
                    lambda: _read_runtime_csv_rows(Path(status["log_csv_path"])),
                    timeout_s=5.0,
                )
                self.assertTrue(rows)
                latest_row = rows[-1]
                self.assertIn("channel0_gpu_airflow_boost_pct", latest_row)
                self.assertGreaterEqual(
                    float(latest_row["channel0_gpu_airflow_boost_pct"]),
                    4.0,
                )

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
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=66.0),
            ):
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
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=59.0),
            ):
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
                    "low_band_residual_cap_pct": 0.25,
                },
                extra_channel_fields={
                    "low_band_stage": 1,
                    "low_band_debt_threshold": 0.05,
                    "low_band_hold_ms": 100,
                    "low_band_max_boost_pct": 3.0,
                },
            )
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=66.0),
            ):
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
                self.assertGreaterEqual(
                    channel["last_low_band_stage_boost_pct"], 0.5
                )
                self.assertLessEqual(
                    channel["last_low_band_effective_boost_pct"], 0.2501
                )
                self.assertGreaterEqual(channel["last_setpoint_pct"], 20.25)
                self.assertLessEqual(channel["last_setpoint_pct"], 20.35)
                self.assertGreaterEqual(channel["low_band_activation_count"], 1)

                rows = _read_runtime_csv_rows(Path(status["log_csv_path"]))
                self.assertTrue(rows)
                latest_row = rows[-1]
                self.assertIn("channel0_low_band_stage_boost_pct", latest_row)
                self.assertIn(
                    "channel0_low_band_effective_boost_pct", latest_row
                )
                self.assertIn("channel0_low_band_debt", latest_row)
                self.assertIn("channel0_low_band_stage_active", latest_row)
                self.assertLessEqual(
                    float(latest_row["channel0_low_band_effective_boost_pct"]),
                    0.2501,
                )

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
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=82.0),
            ):
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

    def test_control_loop_cadence_floor_above_tick_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                poll_tick_ms=200,
                extra_loop_fields={"poll_tick_floor_ms": 250},
            )
            result = _run_control(
                "--mode",
                "control-loop",
                "--config",
                str(config_path),
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "poll_tick_floor_ms must be <= poll_tick_ms",
                result.stderr,
            )

    def test_control_loop_cadence_floor_zero_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                poll_tick_ms=200,
                extra_loop_fields={"poll_tick_floor_ms": 0},
            )
            result = _run_control(
                "--mode",
                "control-loop",
                "--config",
                str(config_path),
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "poll_tick_floor_ms must be > 0",
                result.stderr,
            )

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
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=75.0),
            ):
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
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=75.0),
            ):
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
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env={
                    **_sim_direct_env(channel=0, amd_temp_c=75.0),
                    "SVG_MB_CONTROL_SIM_WRITE_MODE": "policy_refused",
                },
            ):
                failed_event = _wait_for(
                    lambda: (
                        next(
                            (
                                item
                                for item in _read_runtime_events(runtime_home)
                                if item.get("event_type") == "control_loop.write_failed"
                                and item.get("success") is False
                            ),
                            None,
                        )
                        if not _read_pending_writes(runtime_home)
                        else None
                    ),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(
                    failed_event,
                    msg="policy-refused pending write entry was not flushed",
                )
                self.assertEqual(failed_event["severity"], "error")
                self.assertEqual(
                    failed_event["error_code"], "CONTROL_LOOP_WRITE_FAILED"
                )

    def test_control_loop_accepts_operator_breaker_reset(self) -> None:
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
            )
            with RuntimeProbe(
                ["--mode", "control-loop", "--config", str(config_path)],
                env={
                    **_sim_direct_env(channel=0, amd_temp_c=75.0),
                    "SVG_MB_CONTROL_SIM_WRITE_MODE": "fail_immediate",
                },
            ):
                opened = _wait_for(
                    lambda: next(
                        (
                            item
                            for item in _read_runtime_events(runtime_home)
                            if item.get("event_type")
                            == "control_loop.circuit_breaker_opened"
                        ),
                        None,
                    ),
                    timeout_s=6.0,
                )
                self.assertIsNotNone(opened)

                result = _run_control(
                    "--reset-breakers",
                    "--reset-breaker-channel",
                    "0",
                    "--config",
                    str(config_path),
                )
                self.assertEqual(
                    result.returncode,
                    0,
                    msg=f"{result.stdout}\n{result.stderr}",
                )
                self.assertIn("channel: 0", result.stdout)

                reset_event = _wait_for(
                    lambda: next(
                        (
                            item
                            for item in _read_runtime_events(runtime_home)
                            if item.get("event_type")
                            == "control_loop.circuit_breaker_reset"
                        ),
                        None,
                    ),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(reset_event)
                self.assertEqual(reset_event["channel"], 0)
                self.assertEqual(reset_event["severity"], "info")
                self.assertEqual(reset_event["error_code"], "none")
                self.assertTrue(
                    _wait_for(
                        lambda: not (
                            runtime_home
                            / "circuit_breaker_reset.request.json"
                        ).exists(),
                        timeout_s=2.0,
                    )
                )

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
            abort_event = next(
                (
                    item
                    for item in events
                    if item.get("event_type") == "control_loop.abort"
                ),
                None,
            )
            self.assertIsNotNone(abort_event)
            self.assertEqual(abort_event["severity"], "critical")
            self.assertEqual(abort_event["error_code"], "CONTROL_LOOP_ABORT")

    # ---- Adaptive cadence Phase 2 (slew-only; design tests 1-5) ----

    def _run_cadence_loop(
        self,
        td: Path,
        *,
        tctl_sequence: str,
        poll_tick_ms: int = 200,
        floor_ms: int | None = 50,
        min_ticks: int = 30,
        timeout_s: float = 20.0,
        relax_per_s: float = 50.0,
    ):
        runtime_home = td / "runtime"
        extra: dict[str, object] | None = None
        if floor_ms is not None:
            extra = {
                "poll_tick_floor_ms": floor_ms,
                "cadence_slew_start_c_per_s": 0.5,
                "cadence_slew_full_c_per_s": 3.0,
                "cadence_relax_per_s": relax_per_s,
            }
        config_path = _write_control_loop_config(
            td,
            runtime_home=runtime_home,
            channel=0,
            poll_tick_ms=poll_tick_ms,
            write_cooldown_ms=poll_tick_ms,
            deadband_pct=0.25,
            control_hold_ms=0,
            extra_loop_fields=extra,
        )
        with RuntimeProbe(
            ["--mode", "control-loop", "--config", str(config_path)],
            env={
                **_sim_direct_env(channel=0, amd_temp_c=50.0),
                "SVG_MB_CONTROL_SIM_GPU_MODE": "enabled",
                "SVG_MB_CONTROL_SIM_GPU_CORE_C": "50.0",
                "SVG_MB_CONTROL_SIM_GPU_MEMJN_C": "50.0",
                "SVG_MB_CONTROL_SIM_GPU_HOTSPOT_C": "50.0",
                "SVG_MB_CONTROL_SIM_AMD_TCTL_SEQUENCE_C": tctl_sequence,
            },
        ):
            observed = _wait_for(
                lambda: (_read_runtime_status(runtime_home) or {}).get(
                    "loop_tick_count", 0
                )
                >= min_ticks,
                timeout_s=timeout_s,
            )
            self.assertTrue(
                observed, msg="control loop did not reach min_ticks"
            )
            status = _read_runtime_status(runtime_home)
        self.assertIsNotNone(status)
        rows = _read_runtime_csv_rows(Path(status["log_csv_path"]))
        self.assertTrue(rows, msg="cadence CSV rows missing")
        return runtime_home, status, rows

    def test_cadence_steady_state_holds_p(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            _, _, rows = self._run_cadence_loop(
                Path(td_str), tctl_sequence="62.0", min_ticks=25
            )
        intervals = [int(r["loop_intended_interval_ms"]) for r in rows]
        self.assertTrue(
            all(v == 200 for v in intervals),
            msg=f"flat temp must hold P=200, got {sorted(set(intervals))}",
        )
        self.assertTrue(
            all(float(r["cadence_transient"]) < 1e-6 for r in rows)
        )

    def test_cadence_tightens_under_ramp_within_bounds(self) -> None:
        ramp = ",".join([f"{50 + 3 * i}" for i in range(25)] + ["95"] * 40)
        with tempfile.TemporaryDirectory() as td_str:
            _, _, rows = self._run_cadence_loop(
                Path(td_str),
                tctl_sequence=ramp,
                min_ticks=40,
                timeout_s=25.0,
            )
        intervals = [int(r["loop_intended_interval_ms"]) for r in rows]
        self.assertLess(
            min(intervals),
            200,
            msg=f"ramp should shorten interval, got {sorted(set(intervals))}",
        )
        self.assertGreaterEqual(
            min(intervals), 50, msg="interval must not drop below F"
        )
        self.assertLessEqual(
            max(intervals), 200, msg="interval must never exceed P"
        )
        self.assertGreater(
            max(float(r["cadence_transient"]) for r in rows), 0.5
        )

    def test_cadence_relaxes_gradually_after_ramp(self) -> None:
        ramp = ",".join([f"{50 + 4 * i}" for i in range(20)] + ["95"] * 80)
        with tempfile.TemporaryDirectory() as td_str:
            _, _, rows = self._run_cadence_loop(
                Path(td_str),
                tctl_sequence=ramp,
                min_ticks=70,
                timeout_s=30.0,
                relax_per_s=40.0,
            )
        intervals = [int(r["loop_intended_interval_ms"]) for r in rows]
        self.assertLess(
            min(intervals), 150, msg="ramp should tighten cadence"
        )
        # Rate-limited relax produces intervals strictly between F and P
        # rather than an instant F->P snap.
        self.assertTrue(
            any(50 < v < 200 for v in intervals),
            msg=f"expected gradual relax intermediates, got {sorted(set(intervals))}",
        )
        self.assertGreater(
            intervals[-1],
            min(intervals),
            msg="interval should recover after the transient ends",
        )

    def test_cadence_disabled_is_byte_identical(self) -> None:
        ramp = ",".join([f"{50 + 5 * i}" for i in range(20)] + ["95"] * 30)
        with tempfile.TemporaryDirectory() as td_str:
            _, _, rows = self._run_cadence_loop(
                Path(td_str),
                tctl_sequence=ramp,
                poll_tick_ms=200,
                floor_ms=200,
                min_ticks=30,
                timeout_s=20.0,
            )
        intervals = [int(r["loop_intended_interval_ms"]) for r in rows]
        self.assertTrue(
            all(v == 200 for v in intervals),
            msg=f"floor==P must pin interval to P, got {sorted(set(intervals))}",
        )
        self.assertTrue(
            all(float(r["cadence_transient"]) == 0.0 for r in rows),
            msg="disabled cadence must report zero transient",
        )

    def test_cadence_variable_run_ingests_under_current_schema(self) -> None:
        ramp = ",".join([f"{50 + 3 * i}" for i in range(25)] + ["95"] * 40)
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home, _, rows = self._run_cadence_loop(
                td, tctl_sequence=ramp, min_ticks=40, timeout_s=25.0
            )
            intervals = [int(r["loop_intended_interval_ms"]) for r in rows]
            self.assertGreater(
                len(set(intervals)),
                1,
                msg="run should have produced variable cadence",
            )
            db_path = td / "out.db"
            result = _run_control(
                "analyze",
                "ingest",
                "--runtime-home",
                str(runtime_home),
                "--db",
                str(db_path),
                "--quiet",
            )
            self.assertEqual(
                result.returncode,
                0,
                msg=f"{result.stdout}\n{result.stderr}",
            )
            self.assertTrue(
                db_path.is_file() and db_path.stat().st_size > 0
            )
